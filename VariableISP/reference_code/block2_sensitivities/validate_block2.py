"""Generate three numerical fixtures and independently check sensitivities."""
import json, math
from pathlib import Path
import numpy as np
from kepler_reference import *

HERE=Path(__file__).resolve().parent

def at(rho,theta,h,second=False):
    return summarize(solve_transfer(rho,theta,h),rho,theta,h,second)

# 1. Exact circular coast. Hessian is nonzero for fixed geometry, zero along
# the co-moving family. The value, costates and all first derivatives vanish.
r1=at(1.,1.,1.,True)
assert r1['J']<1e-20
assert max(abs(x) for x in r1['p0'])<1e-9
assert np.max(np.abs(r1['F_hessian_departure_duration']))<1e-9

# 2. Stored branch point at the game's kappa=0.0032 budget.
rho=1.01;phase=.005;h=.712023798759808;nf=rho**(-1.5);theta=phase+nf*h
r2=at(rho,theta,h,True)
np.testing.assert_allclose(r2['J'],.0032,rtol=2e-9)
assert r2['J_h_geometry']>0 and r2['F_duration_fixed_departure']<0
assert r2['H_drift']<1e-10 and r2['L_drift']<1e-10

# High-order finite differences of independently solved BVPs.
def derivative(fun,x,delta=2e-5):
    return (fun(x-2*delta)-8*fun(x-delta)+8*fun(x+delta)-fun(x+2*delta))/(12*delta)
checks={}
checks['J_Theta_fd']=derivative(lambda th:at(rho,th,h)['J'],theta)
checks['J_h_fixed_geometry_fd']=derivative(lambda hh:at(rho,theta,hh)['J'],h)
checks['J_rho_fd']=derivative(lambda rr:at(rr,theta,h)['J'],rho)
checks['F_h_moving_fd']=derivative(lambda hh:at(rho,phase+nf*hh,hh)['J'],h)
checks['F_d_fixed_h_fd']=derivative(lambda dd:at(rho,phase+(nf-1)*dd+nf*h,h)['J'],0.)
checks['F_d_fixed_a_fd']=derivative(lambda dd:at(rho,theta-dd,h-dd)['J'],0.)
for calc,truth in [('J_Theta_fd','J_Theta'),('J_h_fixed_geometry_fd','J_h_geometry'),
                   ('J_rho_fd','J_rho'),('F_h_moving_fd','F_duration_fixed_departure'),
                   ('F_d_fixed_h_fd','F_departure_fixed_duration'),
                   ('F_d_fixed_a_fd','F_departure_fixed_arrival')]:
    np.testing.assert_allclose(checks[calc],r2[truth],atol=5e-10,rtol=2e-7)
    checks[calc+'_absolute_error']=abs(checks[calc]-r2[truth])

delta=2e-5
Hp=np.empty((2,2))
for col,(dd,dh) in enumerate([(delta,0),(0,delta)]):
    plus=at(rho,theta+(nf-1)*dd+nf*dh,h+dh)
    minus=at(rho,theta-(nf-1)*dd-nf*dh,h-dh)
    for row,key in enumerate(['F_departure_fixed_duration','F_duration_fixed_departure']):
        Hp[row,col]=(plus[key]-minus[key])/(2*delta)
np.testing.assert_allclose(Hp,r2['F_hessian_departure_duration'],rtol=3e-6,atol=1e-8)
checks['date_duration_hessian_fd']=Hp.tolist()
checks['date_duration_hessian_max_error']=float(np.max(np.abs(Hp-r2['F_hessian_departure_duration'])))

# Fuel maps and scalar planner updates at the same point.
chi=.02;chi_d=.0232;effort=r2['J'];slope=r2['F_duration_fixed_departure']
r2['kappa']=.0032
r2['dh_dkappa']=1/slope
r2['chi_initial']=chi
r2['arrival_mass_fraction']=chi/(chi+effort)
r2['propellant_fraction']=effort/(chi+effort)
r2['propellant_fraction_h_derivative']=chi/(chi+effort)**2*slope
r2['chi_dry']=chi_d
r2['loaded_mass_over_dry_mass']=1/(1-effort/chi_d)
r2['loaded_mass_ratio_h_derivative']=chi_d/(chi_d-effort)**2*slope
r2['departure_newton_shift']=-r2['F_departure_fixed_duration']/r2['F_hessian_departure_duration'][0][0]
r2['stop_time_gradient_if_next_start_acceleration_0p1_0p05']=.1**2+.05**2-r2['uf'][0]**2-r2['uf'][1]**2

# 3. Near-circular family illustrating a concave interval. The rigorous
# global counterexample is the small-epsilon argument plus the independent
# interval certificate; finite-epsilon BVP values are extremal fixtures.
eps=.001;rho3=1+eps
rows=[]
for hh in [10.,10.5,11.]:
    rows.append(at(rho3,eps+rho3**(-1.5)*hh,hh,True))
gap=rows[1]['J']-(rows[0]['J']+rows[2]['J'])/2
assert gap>1e-11
assert rows[1]['F_hessian_departure_duration'][1][1]<0
r3={'epsilon':eps,'rho':rho3,'initial_phase':eps,'samples':rows,
    'jensen_gap':gap,'jensen_gap_over_epsilon_squared':gap/eps**2,
    'finite_epsilon_scope':'Numerically solved regular extremal branch; no finite-epsilon global certificate claimed.'}

result={'units':'R0, T0=sqrt(R0^3/mu), V0=R0/T0; effort scale J0=R0^2/T0^3',
        'costate_convention':'H=p.f-|u|^2 maximized; u=p_v/2; no factor 1/2 in effort',
        'state_order':['rx','ry','vx','vy'],'costate_order':['p_rx','p_ry','p_vx','p_vy'],
        'case1_circular_coast':r1,'case2_kappa_0p0032':r2,'case3_nonconvex_family':r3,
        'validation':checks,'fixture_tolerances':{'atol':2e-10,'rtol':2e-6},
        'notes':'Timing identity and arithmetic fixture tolerances can be tighter than BVP reproduction tolerances.'}
(HERE/'block2_test_cases.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
