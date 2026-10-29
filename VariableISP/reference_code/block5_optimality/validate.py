import json,math
from pathlib import Path
import numpy as np
from scipy.linalg import expm
from scipy.optimize import brentq
from scipy.integrate import quad
from optimality_reference import *

# Independent quadrature tests of all four kernel expressions.
for h,L in [(1.,0.),(.712,3.),(.01,.1),(2.,2.)]:
    w=np.sqrt(L)
    f=lambda s:np.sinh(w*s)/w if w else s
    c=lambda s:np.cosh(w*s)
    truth=[quad(lambda s:f(s)**2,0,h)[0],quad(lambda s:c(s)**2,0,h)[0],
           quad(lambda s:(h-s)*f(s)**2,0,h)[0],quad(lambda s:(h-s)*c(s)**2,0,h)[0]]
    np.testing.assert_allclose(gains(h,L),truth,rtol=2e-12,atol=1e-15)
# Verify the Hessian contraction independently by differencing Dg.
r=np.array([1.2,.3]);p=np.array([.4,-.2]);eps=1e-5
fd=np.column_stack([(gravity_gradient(r+eps*np.eye(2)[i])@p-gravity_gradient(r-eps*np.eye(2)[i])@p)/(2*eps) for i in range(2)])
np.testing.assert_allclose(fd,gravity_hessian_contraction(r,p),rtol=2e-8,atol=1e-9)

class Coast:
    def sol(self,t):
        t=np.asarray(t);z=np.zeros_like(t)
        return np.array([np.cos(t),np.sin(t),-np.sin(t),np.cos(t),z,z,z,z])
h=4*math.pi
case1={'rho':1.,'Theta_unwrapped':h,'h':h,'J_exact':0.,'revolutions':2,
       'classification':'EXACT_GLOBAL_MINIMUM: nonnegative effort and zero control',
       'jacobi':jacobi_diagnostics(Coast(),h)}
assert case1['jacobi']['sampled_scaled_sigma_min']>0

rho=1.01;h=.712023798759808;theta=.005+rho**(-1.5)*h
sol=solve_transfer(rho,theta,h)
num=reconstructed_path(sol,h,endpoints=(circle(1.,0),circle(rho,theta)))
b=num['b_sampled']-1e-5; pmax=num['pmax_sampled']+1e-5
metric=tube_metrics(h,num['U'],b,.08,pmax,num['residual_l2'])
assert metric['containment_margin']>0 and metric['gamma']>0
assert metric['gap_bound']<1e-8
cheap=cheap_lower(circle(1.,0),circle(rho,theta),h,.1,num['U'])
assert 0<cheap['lower_bound']<num['U']
case2=dict(rho=rho,Theta=theta,h=h,nominal=reconstructed_path(sol,h,endpoints=(circle(1.,0),circle(rho,theta))),
           tube_delta=.08,b_input=b,pmax_input=pmax,tube=metric,cheap=cheap,
           jacobi=jacobi_diagnostics(sol,h),
           scope='Floating-point evidence only: sampled extrema padding is NOT a validated enclosure.')
assert case2['jacobi']['sampled_scaled_sigma_min']>0.01
# Independent finite perturbation check of the exact cost-difference identity.
from scipy.interpolate import CubicHermiteSpline
nodes=np.linspace(0,h,41); ys=sol.sol(nodes)
ys[:4,0]=circle(1.,0); ys[:4,-1]=circle(rho,theta)
path=CubicHermiteSpline(nodes,ys[:2].T,ys[2:4].T)
def identity_terms(t):
    r=path(t); v=path(t,1); u=path(t,2)-gravity(r)
    p=sol.sol(t)[4:]; pd=sol.sol(t,1)[4:]
    amp=1e-3; om=2*math.pi/h; direction=np.array([1.,-.3])
    dr=amp*(1-math.cos(om*t))*direction
    dv=amp*om*math.sin(om*t)*direction
    dacc=amp*om*om*math.cos(om*t)*direction
    dg=gravity(r+dr)-gravity(r); du=dacc-dg
    rem=dg-gravity_gradient(r)@dr
    eu=2*u-p[2:]; er=pd[:2]+gravity_gradient(r)@p[2:]; ev=pd[2:]+p[:2]
    lhs=(u+du)@(u+du)-u@u
    rhs=du@du+eu@du-er@dr-ev@dv-p[2:]@rem
    return np.array([lhs,rhs])
vals=np.array([sum(quad(lambda t:identity_terms(t)[i],a,b,epsabs=1e-15)[0]
                   for a,b in zip(nodes[:-1],nodes[1:])) for i in range(2)])
assert abs(vals[0]-vals[1])<1e-12
case2['finite_perturbation_identity_error']=float(abs(vals[0]-vals[1]))
# Convergence check: smaller interpolation error should substantially reduce eta.
coarse=reconstructed_path(sol,h,n=41,endpoints=(circle(1.,0),circle(rho,theta)))
assert coarse['residual_l2'][0]>3*num['residual_l2'][0]

# Two identical scalar clamped-beam accessory problems: NOT a Kepler trajectory.
k=600.; beta=brentq(lambda x:np.cosh(x)*np.cos(x)-1,4.,5.)
tc=beta/k**.25
F=np.zeros((8,8));F[:2,2:4]=np.eye(2);F[2:4,6:8]=np.eye(2)/2
F[4:6,:2]=-2*k*np.eye(2);F[6:8,4:6]=-np.eye(2)
X=lambda t:expm(F*t)[:4,4:]
q=8*math.pi**4-1.5*k
assert q<0 and tc<1
assert np.linalg.svd(X(tc),compute_uv=False)[-1]<1e-12
before=np.linalg.det(X(tc-.01));after=np.linalg.det(X(tc+.01))
assert before>0 and after>0
case3=dict(model='Synthetic planar clamped-beam accessory problem; no Kepler claim',
           k=k,h=1.,first_conjugate_time=tc,conjugate_multiplicity=2,
           Q_over_2_for_z_1_minus_cos_2pi_t=q,
           determinant_before=before,determinant_after=after,
           singular_values_at_conjugacy=np.linalg.svd(X(tc),compute_uv=False).tolist())
# Nonzero elementary bounds: fast circular-endpoint transfer.
s0=circle(1.,0);sf=circle(1.,math.pi);hh=.001
lb=cheap_lower(s0,sf,hh,.1,1e12)
assert lb['position']>1e10
assert dual_moment(s0,sf,hh,.1,np.r_[sf[:2]-s0[:2]-hh*s0[2:],np.zeros(2)])>1e10
out={'scope':'Analytic theorems plus double-precision regression fixtures, no interval-certified BVP.',
     'case1_two_revolution_coast':case1,'case2_budget_branch':case2,
     'case3_conjugate_regression':case3,
     'tests':'kernel quadrature, gravity Hessian finite difference, symplectic checks, finite-perturbation cost identity, reconstruction refinement, positive cheap bounds, even-multiplicity conjugacy'}
Path(__file__).with_name('test_cases.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps({'case1_sigma':case1['jacobi']['sampled_scaled_sigma_min'],'case2':metric,'cheap':cheap,'case3':case3},indent=2))
