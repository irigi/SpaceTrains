"""Planar minimum-effort extremals and exact first/second sensitivities.

All quantities normalized by R0, sqrt(R0^3/mu), sqrt(mu/R0).
Maximizing Hamiltonian H=p_r.v+p_v.g+|p_v|^2/4, u=p_v/2.
Numerical BVP solutions are extremals; no global-optimality claim is implicit.
"""
import math
from pathlib import Path
import sys
import numpy as np
from scipy.integrate import solve_bvp, solve_ivp, quad

J2=np.array([[0.,-1.],[1.,0.]])
J4=np.zeros((4,4));J4[:2,:2]=J2;J4[2:,2:]=J2

def circle(r,theta):
    e=np.array([math.cos(theta),math.sin(theta)])
    return np.r_[r*e, (J2@e)/math.sqrt(r)]

def gravity(r):
    return -r/np.linalg.norm(r)**3

def gravity_gradient(r):
    R=np.linalg.norm(r)
    return -np.eye(2)/R**3+3*np.outer(r,r)/R**5

def gravity_hessian_contraction(r,pv):
    R=np.linalg.norm(r);d=r@pv
    return 3*(np.outer(pv,r)+np.outer(r,pv)+d*np.eye(2))/R**5-15*d*np.outer(r,r)/R**7

def flow(y):
    r=y[:2];v=y[2:4];pr=y[4:6];pv=y[6:8]
    return np.r_[v,gravity(r)+pv/2,-gravity_gradient(r)@pv,-pr]

def jacobian(y):
    G=gravity_gradient(y[:2]);K=gravity_hessian_contraction(y[:2],y[6:8])
    F=np.zeros((8,8));F[:2,2:4]=np.eye(2);F[2:4,:2]=G;F[2:4,6:8]=np.eye(2)/2
    F[4:6,:2]=-K;F[4:6,6:8]=-G;F[6:8,4:6]=-np.eye(2)
    return F

def vector_flow(t,y):
    r=y[:2];v=y[2:4];pr=y[4:6];pv=y[6:8]
    R=np.linalg.norm(r,axis=0);rp=np.sum(r*pv,axis=0)
    return np.vstack([v,-r/R**3+pv/2,pv/R**3-3*r*rp/R**5,-pr])

def hamiltonian(y):
    return float(y[4:6]@y[2:4]+y[6:8]@gravity(y[:2])+y[6:8]@y[6:8]/4)

def angular_costate(y):
    return float(y[4:6]@(J2@y[:2])+y[6:8]@(J2@y[2:4]))

def linear_guess(rho,theta,h,t):
    # Local CW model is only an initial guess for the nonlinear solver.
    sys.path.insert(0,str(Path(__file__).resolve().parent/'support'))
    import cw_reference as cw
    goal=circle(rho,theta-h)
    q=goal[:2]-np.array([1.,0.]);v=goal[2:]-J2@goal[:2]
    target=np.r_[q,v];lam=cw.solve_w(h,target)
    ans=np.zeros((8,len(t)))
    for i,tt in enumerate(t):
        pc=2*cw.phi(h-tt).T@lam
        s=np.zeros(4) if tt==0 else cw.gramian(tt)@cw.phi(h-tt).T@lam
        Q=np.array([[math.cos(tt),-math.sin(tt)],[math.sin(tt),math.cos(tt)]])
        ans[:2,i]=Q@(np.array([1.,0.])+s[:2])
        ans[2:4,i]=Q@(s[2:]+J2@(np.array([1.,0.])+s[:2]))
        ans[4:6,i]=Q@(pc[:2]+J2@pc[2:])
        ans[6:8,i]=Q@pc[2:]
    return ans

def solve_transfer(rho,theta,h,tol=2e-11,guess=None):
    if rho<=0 or h<=0:raise ValueError('positive rho and h required')
    s0=circle(1.,0.);sf=circle(rho,theta)
    mesh=np.linspace(0,h,max(80,int(35*h)))
    initial=linear_guess(rho,theta,h,mesh) if guess is None else guess.sol(mesh/ h*guess.x[-1])
    bc=lambda ya,yb:np.r_[ya[:4]-s0,yb[:4]-sf]
    sol=solve_bvp(vector_flow,bc,mesh,initial,tol=tol,max_nodes=30000)
    if not sol.success:raise RuntimeError(sol.message)
    return sol

def summarize(sol,rho,theta,h,second=True):
    y0=sol.sol(0);yf=sol.sol(h)
    effort=quad(lambda t:float(sol.sol(t)[6:8]@sol.sol(t)[6:8]/4),0,h,
                epsabs=1e-14,epsrel=3e-12,limit=200)[0]
    H=hamiltonian(y0);ell=angular_costate(y0);nf=rho**(-1.5)
    a0sq=float(y0[6:8]@y0[6:8]/4);afsq=float(yf[6:8]@yf[6:8]/4)
    er=np.array([math.cos(theta),math.sin(theta)]);et=J2@er
    drho=np.r_[er,-.5*rho**(-1.5)*et]
    out=dict(rho=rho,Theta=theta,h=h,J=effort,H=H,L=ell,
             p0=y0[4:].tolist(),pf=yf[4:].tolist(),u0=(y0[6:8]/2).tolist(),
             uf=(yf[6:8]/2).tolist(),J_rho=float(yf[4:]@drho),J_Theta=ell,J_h_geometry=-H,
             F_departure_fixed_arrival=a0sq,F_arrival_fixed_departure=-afsq,
             F_departure_fixed_duration=a0sq-afsq,F_duration_fixed_departure=-afsq,
             min_radius=float(np.min(np.linalg.norm(sol.sol(np.linspace(0,h,1001))[:2],axis=0))),
             boundary_residual=float(np.linalg.norm(yf[:4]-circle(rho,theta))))
    grid=sol.sol(np.linspace(0,h,101))
    out['H_drift']=max(abs(hamiltonian(y)-H) for y in grid.T)
    out['L_drift']=max(abs(angular_costate(y)-ell) for y in grid.T)
    if not second:return out
    res=solve_ivp(lambda t,m:(jacobian(sol.sol(t))@m.reshape(8,8)).ravel(),[0,h],
                  np.eye(8).ravel(),method='DOP853',rtol=2e-12,atol=2e-13)
    M=res.y[:,-1].reshape(8,8);Mxp=M[:4,4:];Mpp=M[4:,4:]
    sf=circle(rho,theta);s_theta=J4@sf
    p0theta=np.linalg.solve(Mxp,s_theta)
    p0h=np.linalg.solve(Mxp,-flow(yf)[:4])
    pftheta=Mpp@p0theta
    pfh=Mpp@p0h+flow(yf)[4:]
    E_tt=float((J4@y0[:4])@p0theta)
    E_th=float(s_theta@pfh)
    E_hh=float(-flow(y0)[:4]@p0h)
    E_ht=float(-flow(y0)[:4]@p0theta)
    S=np.array([[E_tt,E_th],[E_th,E_hh]])
    alpha=nf-1;Q=np.array([[alpha,nf],[0.,1.]])
    # Moving-arrival derivative: sf_h - f_controlled(sf) = -B*u_f.
    # Combining RHSs first avoids cancellation of O(1) geometric Hessians.
    rhs=np.r_[np.zeros(2),-yf[6:8]/2]
    p0move=np.linalg.solve(Mxp,rhs)
    pfmove=Mpp@p0move+flow(yf)[4:]
    L_h_moving=float((J4@y0[:4])@p0move)
    Fhh=float(-yf[6:8]@pfmove[2:]/2)
    stable=np.array([[alpha*alpha*E_tt,alpha*L_h_moving],
                     [alpha*L_h_moving,Fhh]])
    out.update(E_ThetaTheta=E_tt,E_Thetah=E_th,E_hh=E_hh,
               cross_symmetry_error=abs(E_th-E_ht),
               F_hessian_departure_duration=stable.tolist(),
               hessian_transform_check=float(np.max(np.abs(stable-Q.T@S@Q))),
               L_h_moving=L_h_moving,
               Mxp_condition_number=float(np.linalg.cond(Mxp)),
               p0_Theta=p0theta.tolist(),p0_h=p0h.tolist())
    return out
