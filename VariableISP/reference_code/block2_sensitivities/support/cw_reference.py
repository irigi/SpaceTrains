"""Planar CW reference formulas. All inputs use R0, 1/n, n R0 units.

Research/reference implementation; scipy is used only for validation and roots.
The hot path is a closed Gramian plus a scaled 4x4 Cholesky solve.
"""
import math
import numpy as np

A = np.array([[0.,0.,1.,0.], [0.,0.,0.,1.],
              [3.,0.,0.,2.], [0.,0.,-2.,0.]])
B = np.vstack([np.zeros((2,2)), np.eye(2)])

def phi(h):
    s, c = math.sin(h), math.cos(h)
    d = 2*math.sin(h/2)**2
    # Stable sin(h)-h near zero.
    if abs(h) < .1:
        smh = sum((-1.)**k*h**(2*k+1)/math.factorial(2*k+1)
                  for k in range(1,9))
    else:
        smh = s-h
    return np.array([[1+3*d,0,s,2*d], [6*smh,1,-2*d,h+4*smh],
                     [3*s,0,c,2*s], [-6*d,0,-2*s,1-4*d]])

def scale(h):
    if h <= 0:
        raise ValueError('h must be positive')
    return np.array([h**1.5,h**1.5,h**.5,h**.5])

def scaled_gramian(h):
    """D^-1 W D^-1. Small-h power series avoids cancellation."""
    d = scale(h)
    if h < .1:
        # E_j = h^(j+1/2) D^-1 A^j B / j!.
        # Top two rows of E_0 are exactly zero, so no division by h.
        e = np.vstack([np.zeros((2,2)),np.eye(2)])
        coeff = [e]
        at = np.diag(1/d) @ A @ np.diag(d)
        for j in range(1,17):
            e = h*(at@e)/j
            coeff.append(e)
        w = np.zeros((4,4))
        for i,e in enumerate(coeff):
            for j,f in enumerate(coeff):
                w += (e @ f.T)/(i+j+1)
        return (w+w.T)/2
    s,c=math.sin(h),math.cos(h)
    s2=math.sin(2*h)
    w=np.zeros((4,4))
    w[0,0]=6.5*h-8*s+.75*s2
    w[0,1]=-3*(h-s)**2
    w[0,2]=4*(1-c)-1.5*s*s
    w[0,3]=-11*h+14*s-1.5*s2
    w[1,1]=3*h**3+14*h+24*h*c-32*s-3*s2
    w[1,2]=5*h+6*h*c-8*s-1.5*s2
    w[1,3]=6*s*s+4*(1-c)-12*h*s+4.5*h*h
    w[2,2]=2.5*h-.75*s2
    w[2,3]=3*s*s+6*(c-1)
    w[3,3]=19*h+3*s2-24*s
    w=w+w.T-np.diag(w.diagonal())
    return w/np.outer(d,d)

def gramian(h):
    d=scale(h)
    return scaled_gramian(h)*np.outer(d,d)

def solve_w(h,y):
    d=scale(h)
    l=np.linalg.cholesky(scaled_gramian(h))
    v=np.linalg.solve(l,np.asarray(y)/d)
    return np.linalg.solve(l.T,v)/d

def correction_data(h,z):
    y=phi(h)@np.asarray(z)
    lam=solve_w(h,y)
    return float(y@lam),lam

def correction(t,h,lam):
    return B.T@phi(h-t).T@lam

def relative_path(t,h,z,lam):
    if t==0:
        return np.asarray(z,dtype=float)
    return phi(t)@z - gramian(t)@phi(h-t).T@lam

def cost(h,z):
    return correction_data(h,z)[0]

def cost_derivative(h,z):
    _,lam=correction_data(h,z)
    ah=correction(h,h,lam)
    return -float(ah@ah)

def game_time(z,bp,be):
    from scipy.optimize import brentq
    if min(bp,be)<0:
        raise ValueError('budgets must be nonnegative')
    if np.linalg.norm(z)==0:
        return 0.
    if bp<=be:
        return math.inf
    gap=(bp-be)/(math.sqrt(bp)+math.sqrt(be))
    hi=1.
    while cost(hi,z)>gap*gap:
        hi*=2
        if not math.isfinite(hi):
            raise OverflowError('root outside floating point range')
    lo=hi/2
    while cost(lo,z)<=gap*gap:
        hi,lo=lo,lo/2
    return brentq(lambda h: cost(h,z)-gap*gap,lo,hi,
                  xtol=1e-14,rtol=2e-14)

def kernel_bounds(h):
    g=h+h*h+(2/3)*h**3
    g1=h*h/2+h**3/3+h**4/6
    g2=h**3/3+h**4/2+7*h**5/15+2*h**6/9+4*h**7/63
    return g,g1,g2

def ball_envelope(h,state):
    q=np.asarray(state[:2]); v=np.asarray(state[2:])
    g,_,_=kernel_bounds(h)
    return float(np.linalg.norm(q)+(1.5*h*h+h**3)*abs(q[0])+g*np.linalg.norm(v))

def nonlinear_certificate(h,z,evader_state,be,eps):
    if not 0<eps<1 or be<0:
        raise ValueError('requires 0<eps<1 and be>=0')
    c=cost(h,z)
    _,g1,g2=kernel_bounds(h)
    d=ball_envelope(h,z)+math.sqrt(c*g2)
    e=ball_envelope(h,evader_state)+math.sqrt(be*g2)
    nmax=3*eps*eps/(1-eps)**4
    lip=6*eps/(1-eps)**4
    tube=e+d+nmax*g1
    gamma=lip*math.sqrt(h)*d
    required=(math.sqrt(be)+math.sqrt(c)+gamma)**2
    return dict(C=c,D=d,E=e,Nmax=nmax,L=lip,G1=g1,G2=g2,
                tube_lhs=tube,tube_valid=bool(tube<eps),Gamma=gamma,
                required_bp=required,
                outer_budget=(math.sqrt(be)+nmax*math.sqrt(h))**2)

def remainder(q):
    q=np.asarray(q)
    x=q+np.array([1.,0.])
    return -x/np.linalg.norm(x)**3+np.array([1.,0.])-np.array([2.,-1.])*q

def inertial_fallback(h,r,w,XE,VE,be,rstar,normal):
    r,w,XE,VE,normal=map(np.asarray,(r,w,XE,VE,normal))
    if abs(np.linalg.norm(normal)-1)>1e-12 or rstar<=0 or h<=0:
        raise ValueError('unit normal, positive h and rstar required')
    c=12*(r@r)/h**3+12*(r@w)/h**2+4*(w@w)/h
    ij=13*h*(r@r)/35+11*h*h*(r@w)/105+h**3*(w@w)/105
    d=np.linalg.norm(r)+4*h*np.linalg.norm(w)/27
    corridor=min(normal@XE,normal@(XE+h*VE))-h*h/(2*rstar*rstar)-math.sqrt(be*h**3/3)-d
    required=(math.sqrt(be)+math.sqrt(c)+2*math.sqrt(ij)/rstar**3)**2
    return dict(C=float(c),I=float(ij),D=float(d),corridor_lhs=float(corridor),
                corridor_valid=bool(corridor>rstar),required_bp=float(required))
