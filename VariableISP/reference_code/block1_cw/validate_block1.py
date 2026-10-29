"""Independent numerical checks and machine-readable unit-test fixtures."""
import json, math
from pathlib import Path
import numpy as np
from scipy.integrate import quad_vec, solve_ivp
from scipy.linalg import expm
from cw_reference import *

HERE=Path(__file__).resolve().parent
checks={}
maxerr=0.
for h in [1e-6,.001,.01,.099,.1,.5,1.,math.pi,10.,100.]:
    np.testing.assert_allclose(phi(h),expm(h*A),atol=2e-10,rtol=2e-10)
    ref=quad_vec(lambda t:(expm(t*A)@B)@(expm(t*A)@B).T,
                 0,h,epsabs=1e-13,epsrel=2e-13)[0]
    err=np.max(np.abs((gramian(h)-ref)/np.outer(scale(h),scale(h))))
    maxerr=max(maxerr,err)
    assert err<2e-10
checks['max_scaled_gramian_error_vs_expm_quadrature']=maxerr

z1=np.array([1e-5,0.,0.,0.]);h1=.01;be1=.0003
c1,l1=correction_data(h1,z1)
bp1=(math.sqrt(be1)+math.sqrt(c1))**2
z2=np.array([.01,-.02,.003,-.004]);bp2=.0032;be2=.0008
h2=game_time(z2,bp2,be2);c2,l2=correction_data(h2,z2)
for z,h,c,lam in [(z1,h1,c1,l1),(z2,h2,c2,l2)]:
    integ=quad_vec(lambda t:np.array([correction(t,h,lam)@correction(t,h,lam)]),0,h)[0][0]
    np.testing.assert_allclose(integ,c,rtol=2e-10)
    sol=solve_ivp(lambda t,z:A@z-B@correction(t,h,lam),[0,h],z,
                  rtol=2e-12,atol=1e-14)
    assert np.linalg.norm(sol.y[:,-1])<2e-11
    dh=1e-4*h
    fd=(cost(h-2*dh,z)-8*cost(h-dh,z)+8*cost(h+dh,z)-cost(h+2*dh,z))/(12*dh)
    np.testing.assert_allclose(fd,cost_derivative(h,z),rtol=2e-7,atol=1e-10)
checks['correction_energy_endpoint_and_derivative']='passed'
assert math.isinf(game_time(z2,.0032,.0032))
assert game_time(np.zeros(4),0,0)==0

# Position ellipsoid boundary and fixed-final-state effort agree with W.
w2=gramian(h2); normal=np.array([1.,2.])
dr=math.sqrt(bp2/(normal@w2[:2,:2]@normal))*(w2[:2,:2]@normal)
metric=float(dr@np.linalg.solve(w2[:2,:2],dr))
np.testing.assert_allclose(metric,bp2,rtol=1e-12)

# Both near-field and wide-separation nonlinear certificates.
h3=.2;z3=np.array([.05,0.,0.,0.]);be3=1e-5;eps=.25
cert=nonlinear_certificate(h3,z3,np.zeros(4),be3,eps)
assert cert['tube_valid']
c3,l3=correction_data(h3,z3)
def evader(t):
    return np.array([math.sqrt(2*be3/h3)*math.sin(math.pi*t/h3),0.])
def rhs(t,y):
    se,sp=y[:4],y[4:8]
    ue=evader(t)
    up=ue+correction(t,h3,l3)+remainder(se[:2])-remainder(sp[:2])
    return np.r_[A@se+B@(ue+remainder(se[:2])),
                 A@sp+B@(up+remainder(sp[:2])),ue@ue,up@up]
y0=np.r_[np.zeros(4),-z3,0.,0.]
sol=solve_ivp(rhs,[0,h3],y0,rtol=2e-12,atol=1e-14,dense_output=True)
end=sol.y[:,-1]
err=float(np.linalg.norm(end[:4]-end[4:8]))
assert err<2e-10
np.testing.assert_allclose(end[8],be3,rtol=1e-9)
assert end[9]<cert['required_bp']
checks['nonlinear_compensation_terminal_error']=err
checks['nonlinear_sample_pursuer_effort']=float(end[9])
checks['nonlinear_sample_evader_effort']=float(end[8])
patherr=0.
for t in np.linspace(0,h3,101):
    yy=sol.sol(t)
    patherr=max(patherr,float(np.linalg.norm(yy[:4]-yy[4:8]-relative_path(t,h3,z3,l3))))
checks['nonlinear_sample_max_relative_path_error']=patherr

wide=inertial_fallback(.2,[.4,0],[0,.4],[1,0],[0,1],1e-5,.5,[1,0])
assert wide['corridor_valid']

# Remainder inequalities are sampled checks, not the proof of certification.
rng=np.random.default_rng(1729)
for _ in range(1000):
    theta=rng.uniform(0,2*math.pi,2)
    radii=eps*np.sqrt(rng.uniform(0,1,2))
    q=np.c_[np.cos(theta),np.sin(theta)]*radii[:,None]
    assert np.linalg.norm(remainder(q[0])) <= cert['Nmax']*(1+1e-12)
    assert np.linalg.norm(remainder(q[0])-remainder(q[1])) <= cert['L']*np.linalg.norm(q[0]-q[1])*(1+1e-12)
checks['sampled_remainder_bounds']='1000 pairs passed; analytical proof in paper'

def fixture(h,z,be,bp,lam):
    return dict(h=h,z0=z.tolist(),b_E=be,b_P=bp,C=cost(h,z),
                Cprime=cost_derivative(h,z),W=gramian(h).tolist(),
                lambda_terminal=lam.tolist(),
                a_at_0=correction(0,h,lam).tolist(),
                a_at_half=correction(h/2,h,lam).tolist(),
                a_at_h=correction(h,h,lam).tolist(),
                z_at_half=relative_path(h/2,h,z,lam).tolist())
cases={
 'units':'R0, 1/n, n*R0, n^2*R0; budgets divided by n^3*R0^2',
 'state_order':['x','y','dx_dtau','dy_dtau'],
 'recommended_tolerance':{'rtol':2e-9,'atol':2e-12},
 'case_1_short_horizon':fixture(h1,z1,be1,bp1,l1),
 'case_2_atlas_budget':fixture(h2,z2,be2,bp2,l2),
 'case_3_nonlinear_certificate':dict(h=h3,z0=z3.tolist(),evader_state0=[0]*4,
                                  b_E=be3,epsilon=eps,b_P_test=5.5,**cert),
 'case_3b_wide_separation_fallback':dict(h=.2,r=[.4,0],w=[0,.4],X_E=[1,0],
                                       V_E=[0,1],b_E=1e-5,r_star=.5,
                                       normal=[1,0],b_P_test=301.,**wide),
 'case_2_position_ellipsoid':dict(normal=normal.tolist(),boundary_displacement=dr.tolist(),metric=metric),
 'edge_cases':{'nonzero_state_equal_budgets':'infinite','zero_state_zero_budgets':0},
 'validation':checks}
(HERE/'cw_test_cases.json').write_text(json.dumps(cases,indent=2)+'\n')
print(json.dumps(cases,indent=2))
