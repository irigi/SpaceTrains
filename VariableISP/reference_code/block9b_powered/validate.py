"""Finite powered SOI pass: feasible reference, not a certified optimum."""
import math,json
from pathlib import Path
import numpy as np
from scipy.integrate import solve_ivp,quad
from scipy.optimize import minimize_scalar
J2=np.array([[0.,-1.],[1.,0.]])
def asymptote(x,v,incoming):
 r=np.linalg.norm(x);E=v@v/2-1/r
 if E<=0:raise ValueError('nonhyperbolic endpoint')
 s=math.sqrt(2*E);h=float(x[0]*v[1]-x[1]*v[0])
 ev=((v@v-1/r)*x-(x@v)*v);e=np.linalg.norm(ev);eh=ev/e
 return s/e*((1 if incoming else -1)*eh+h*s*(J2@eh))
def run(acc):
 def rhs(t,y):
  x=y[:2];v=y[2:4];a=acc*v/np.linalg.norm(v)
  return np.r_[v,-x/np.linalg.norm(x)**3+a,acc**2,v@a,x[0]*a[1]-x[1]*a[0]]
 def out(t,y):return np.linalg.norm(y[:2])-1
 out.direction=1;out.terminal=True
 sol=solve_ivp(rhs,[0,5],[1,0,-1.6,.5,0,0,0],events=out,rtol=2e-12,atol=2e-13,method='DOP853',dense_output=True,max_step=.01)
 assert len(sol.t_events[0])==1
 T=float(sol.t[-1]);yf=sol.y[:,-1];v0=np.array([-1.6,.5]);x0=np.array([1.,0.])
 rmin=minimize_scalar(lambda t:np.linalg.norm(sol.sol(t)[:2]),bounds=(0,T),method='bounded',options={'xatol':1e-14})
 wi=asymptote(x0,v0,True);wo=asymptote(yf[:2],yf[2:4],False)
 angle=math.atan2(wi[0]*wo[1]-wi[1]*wo[0],wi@wo)
 E0=v0@v0/2-1;Ef=yf[2:4]@yf[2:4]/2-1
 err=abs(Ef-E0-yf[5]);assert err<2e-10
 hfinal=yf[0]*yf[3]-yf[1]*yf[2];assert abs(hfinal-.5-yf[6])<2e-11
 I=float(yf[4]);energy_lb=(math.sqrt(2*Ef+20)-math.sqrt(2*E0+20))**2/T
 assert I+1e-12>=energy_lb
 if acc:
  fraction=quad(lambda t:float(sol.sol(t)[2:4]@(acc*sol.sol(t)[2:4]/np.linalg.norm(sol.sol(t)[2:4]))) if np.linalg.norm(sol.sol(t)[:2])<.2 else 0,0,T,epsabs=1e-10,points=[float(rmin.x)],limit=300)[0]/yf[5]
 else:fraction=0
 inner_time=quad(lambda t:1. if np.linalg.norm(sol.sol(t)[:2])<.2 else 0.,0,T,epsabs=1e-10,points=[float(rmin.x)],limit=300)[0]/T
 return dict(acceleration=acc,duration=T,exit_position=yf[:2].tolist(),exit_velocity=yf[2:4].tolist(),min_radius=float(rmin.fun),periapsis_time=float(rmin.x),incoming_excess=wi.tolist(),outgoing_excess=wo.tolist(),s_in=float(np.linalg.norm(wi)),s_out=float(np.linalg.norm(wo)),turn=angle,J=I,energy_work=float(yf[5]),angular_work=float(yf[6]),energy_identity_error=err,energy_lower_bound=energy_lb,energy_fraction_inside_radius_point2=fraction,time_fraction_inside_radius_point2=inner_time)
coast=run(0);powered=run(.02)
sin=coast['s_in'];ah=1/sin**2;e=math.sqrt(1+sin*sin*.25);F=math.acosh((1/ah+1)/e)
T=2*math.sqrt(ah**3)*(e*math.sinh(F)-F)
assert abs(T-coast['duration'])<1e-10
assert abs(coast['turn']-2*math.asin(1/e))<1e-10
powered.update(chi=.4,nu_minus=10,nu_plus=50,kappa=.0032,remaining_budget=.0032-powered['J'],fuel_fraction=powered['J']/(.4+powered['J']))
assert powered['min_radius']>.1 and powered['remaining_budget']>0
assert (.4+powered['J'])/50<.02<.4/10
# Global SOI guard: radius=1, planet at (10,0), inward crossing x=(0,1).
Rp=np.array([10.,0.]);Vp=np.array([0.,math.sqrt(.1)]);Ap=np.array([-.01,0.]);x=np.array([0.,1.]);v=np.array([.5,-2.]);X=Rp+x;V=Vp+v
gm=-X/np.linalg.norm(X)**3;gp=Ap-.01*x;dg=gp-gm;pv=np.array([.3,.4]);prm=np.array([.1,-.2]);den=x@v
eta=-pv@dg/den;prp=prm+eta*x
Hm=prm@V+pv@gm+pv@pv/4;Hp=prp@V+pv@gp+pv@pv/4
ct=-x@Vp
assert abs(Hp-Hm+eta*ct)<1e-14
S=np.eye(4)+np.outer(np.r_[np.zeros(2),dg],np.r_[x,np.zeros(2)])/den
assert np.max(np.abs(np.r_[prm,pv]-S.T@np.r_[prp,pv]))<1e-14
out={'case1_ballistic':coast,'case2_powered_feasible':powered,'case3_soi_junction':{'planet_position':Rp.tolist(),'planet_velocity':Vp.tolist(),'planet_acceleration':Ap.tolist(),'relative_position':x.tolist(),'relative_velocity':v.tolist(),'g_before':gm.tolist(),'g_after':gp.tolist(),'p_v':pv.tolist(),'p_r_minus':prm.tolist(),'p_r_plus':prp.tolist(),'guard_multiplier':float(eta),'H_minus':float(Hm),'H_plus':float(Hp),'guard_time_derivative':float(ct),'saltation':S.tolist()}}
Path(__file__).with_name('test_cases.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
