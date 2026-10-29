"""Block 5: numerical diagnostics, not outward-rounded certificates."""
import sys
from pathlib import Path
import numpy as np
from scipy.integrate import solve_ivp, quad
from scipy.interpolate import CubicHermiteSpline
sys.path.insert(0, str(Path(__file__).parent/'support'))
from kepler_reference import (circle, gravity, gravity_gradient,
    gravity_hessian_contraction, jacobian, solve_transfer)


def gains(h, L):
    """Squared L2-to-point gains K_r^2,K_v^2; integrated gains A_r,A_v."""
    if h <= 0 or L < 0: raise ValueError('h>0, L>=0 required')
    # Positive series avoids catastrophic cancellation at short horizons.
    import math
    x=L*h*h
    if not np.isfinite(x) or x>100: raise ValueError("scaled horizon too large for diagnostic kernel")
    kr=kv=ar=av=0.
    for j in range(40):
        # sinh(sqrt(L)*t)^2/L = sum 2^(2j+1)*L^j*t^(2j+2)/(2j+2)!
        c=2.**(2*j+1)*x**j/math.factorial(2*j+2)
        kr += c/(2*j+3)
        ar += c/((2*j+3)*(2*j+4))
        # cosh(sqrt(L)*t)^2 = 1 + sum 2^(2j+1)*L^(j+1)*t^(2j+2)/(2j+2)!
        kv += c*x/(2*j+3)
        av += c*x/((2*j+3)*(2*j+4))
    return np.array([h**3*kr, h*(1+kv), h**4*ar, h*h*(.5+av)])


def tube_metrics(h, U, b, delta, pmax, residual_l2=(0.,0.,0.)):
    """residual_l2 = (||2u-pv||, ||pr'+G pv||, ||pv'+pr||)."""
    if min(U,pmax)<0 or not 0<delta<b: raise ValueError('invalid tube data')
    ell=b-delta; L=2/ell**3
    kr,kv,ar,av=gains(h,L)
    c=12*pmax/ell**4
    gamma=1-c*ar
    margin=delta-2*np.sqrt(U*kr)
    eu,er,ev=residual_l2
    if min(eu,er,ev)<0: raise ValueError('negative residual norm')
    eta=eu+er*np.sqrt(ar)+ev*np.sqrt(av)
    gap=eta*eta/(4*gamma) if gamma>0 and margin>0 else None
    return dict(L=L, Kr2=kr, Kv2=kv, Ar=ar, Av=av,
                containment_margin=margin, gamma=gamma, eta=eta,
                gap_bound=gap, lower_bound=max(0.,U-gap) if gap is not None else None)


def cheap_lower(s0,sf,h,a,U):
    """Analytic lower bound for floor a and any nonnegative cutoff U.
    U need not be feasible for the lower bound, but must be for a gap claim.
    """
    if h<=0 or a<=0 or U<0: raise ValueError('invalid bound inputs')
    r0,v0=np.asarray(s0[:2]),np.asarray(s0[2:])
    rf,vf=np.asarray(sf[:2]),np.asarray(sf[2:])
    if min(np.linalg.norm(r0),np.linalg.norm(rf))<a: raise ValueError('endpoints below floor')
    gm=a**-2; dv=vf-v0; dr=rf-r0-h*v0
    lv=max(0.,np.linalg.norm(dv)-gm*h)**2/h
    lr=max(0.,np.linalg.norm(dr)-gm*h*h/2)**2/(h**3/3)
    E0=v0@v0/2-1/np.linalg.norm(r0); Ef=vf@vf/2-1/np.linalg.norm(rf)
    c=v0@v0+2/a-2/np.linalg.norm(r0)
    vint=(h*np.sqrt(U)+np.sqrt(h*h*U+h*c))**2
    le=(Ef-E0)**2/vint if vint>0 else 0.
    rmax=np.linalg.norm(r0)+h*np.linalg.norm(v0)+gm*h*h/2+np.sqrt(U*h**3/3)
    cross=lambda r,v:r[0]*v[1]-r[1]*v[0]
    lm=(cross(rf,vf)-cross(r0,v0))**2/(h*rmax*rmax)
    return dict(velocity=lv,position=lr,energy=le,angular=lm,
                lower_bound=max(lv,lr,min(U,max(le,lm))))


def dual_moment(s0,sf,h,a,lam,segments=16):
    """Every lam is valid; convex trapezoidal upper bound for gravity support."""
    lam=np.asarray(lam); alpha,beta=lam[:2],lam[2:]
    d=np.r_[sf[:2]-s0[:2]-h*s0[2:], sf[2:]-s0[2:]]
    denom=(h**3/3)*(alpha@alpha)+h*h*(alpha@beta)+h*(beta@beta)
    if denom<=0: return 0.
    t=np.linspace(0,h,segments+1)
    q=(h-t[:,None])*alpha+beta
    f=np.linalg.norm(q,axis=1)
    support=h/segments*(f.sum()-(f[0]+f[-1])/2)
    return max(0.,lam@d-a**-2*support)**2/denom


def jacobi_diagnostics(sol,h,n=500):
    init=np.vstack([np.zeros((4,4)),np.eye(4)])
    ivp=solve_ivp(lambda t,z:(jacobian(sol.sol(t))@z.reshape(8,4)).ravel(),
                  [0,h],init.ravel(),method='DOP853',rtol=2e-11,atol=2e-13,dense_output=True)
    if not ivp.success: raise RuntimeError(ivp.message)
    mins=[]; sym=[]
    for t in np.linspace(max(1e-4,h/10000),h,n):
        z=ivp.sol(t).reshape(8,4); X,Y=z[:4],z[4:]
        dl=np.diag([t**-1.5]*2+[t**-.5]*2)
        dr=np.diag([t**-1.5]*2+[t**-.5]*2)
        mins.append(np.linalg.svd(dl@X@dr,compute_uv=False)[-1])
        sym.append(np.linalg.norm(X.T@Y-Y.T@X)/(1+np.linalg.norm(X)*np.linalg.norm(Y)))
    z=ivp.sol(h).reshape(8,4);X,Y=z[:4],z[4:]
    S=np.linalg.solve(X.T,Y.T).T
    return dict(X_final=X.tolist(),Y_final=Y.tolist(),
                sampled_scaled_sigma_min=min(mins),symplectic_error=max(sym),
                riccati_symmetry_error=float(np.linalg.norm(S-S.T)),
                status='SAMPLED_DIAGNOSTIC_ONLY')


def reconstructed_path(sol,h,n=81,endpoints=None):
    t=np.linspace(0,h,n); y=sol.sol(t)
    if endpoints is not None:
        y[:4,0]=endpoints[0]; y[:4,-1]=endpoints[1]
    # C1 cubic positions with explicitly prescribed endpoint velocities.
    spline=CubicHermiteSpline(t,y[:2].T,y[2:4].T)
    def state(t): return np.r_[spline(t),spline(t,1)]
    def control(t): return spline(t,2)-gravity(spline(t))
    def integrate(fun):
        return sum(quad(fun,l,r,epsabs=1e-16,epsrel=2e-10)[0] for l,r in zip(t[:-1],t[1:]))
    U=integrate(lambda t:float(control(t)@control(t)))
    def residuals(t):
        p=sol.sol(t)[4:]; pd=sol.sol(t,1)[4:]
        return (2*control(t)-p[2:],pd[:2]+gravity_gradient(spline(t))@p[2:],pd[2:]+p[:2])
    norms=[np.sqrt(integrate(lambda t,i=i:float(residuals(t)[i]@residuals(t)[i]))) for i in range(3)]
    ts=np.linspace(0,h,4001)
    b=float(np.min(np.linalg.norm(spline(ts),axis=1)))
    pmax=float(np.max(np.linalg.norm(sol.sol(ts)[6:8],axis=0)))
    return dict(U=U,b_sampled=b,pmax_sampled=pmax,residual_l2=norms,
                knots=n,endpoint_residual=float(np.linalg.norm(state(h)-y[:4,-1])))
