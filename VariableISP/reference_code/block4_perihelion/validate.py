import math,json
from pathlib import Path
import numpy as np
from scipy.integrate import solve_ivp
R=.1;z=3.2;A=1/R**2-z*z/R;L=-4*A*z;omega=z/R
mu=-2*A*(2+R*z*z)/R**4
# Exact constant-speed circular boundary: integrate full state and measure-density adjoint.
y0=[R,0,0,z,0,-2*A*z/R,2*A,0]
def rhs(t,y):
 r=y[:2];v=y[2:4];pr=y[4:6];pv=y[6:8];norm=np.linalg.norm(r)
 G=-np.eye(2)/norm**3+3*np.outer(r,r)/norm**5
 return np.r_[v,-r/norm**3+pv/2,-G@pv-mu*r,-pr]
sol=solve_ivp(rhs,[0,.0005],y0,rtol=2e-12,atol=1e-13)
t=.0005;er=np.array([math.cos(omega*t),math.sin(omega*t)]);et=np.array([-er[1],er[0]])
exact=np.r_[R*er,z*et,-2*A*z/R*et,2*A*er]
err=float(np.max(np.abs(sol.y[:,-1]-exact)))
assert err<2e-7
out={'case1':{'radius':R,'tangential_speed':math.sqrt(10),'duration':.02,'angle_change':math.sqrt(10)/R*.02,'effort':0,'status':'globally minimal boundary coast for its own boundary endpoints'},
'case2':{'radius':R,'tangential_speed':z,'radial_thrust':A,'angular_costate':L,'multiplier_density':mu,'duration':t,'effort':A*A*t,'kappa':.0032,'remaining_budget':.0032-A*A*t,'full_ode_max_error':err,'band_a_min':.8,'band_a_max':4,'allowed_abs_tangent':[0,3.2],'status':'necessary-condition fixture; no global optimality claim'},
'case3':{'safe_line_y':.101,'safe_line_x_endpoints':[-.02,.02],'unsafe_cubic_x_controls':[.12,.08,.08,.12],'unsafe_cubic_midpoint_radius':.09,'radius_limit':.1,'position_error_bound':1e-5}}
Path(__file__).with_name('test_cases.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
