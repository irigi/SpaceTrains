"""Independent normalized Cartesian shooting, seeded by pinned public atlas.
Does not use the repository's permissive endpoint acceptance test.
"""
import numpy as np,json,sys
from pathlib import Path
from scipy.integrate import solve_ivp,cumulative_trapezoid
from scipy.optimize import least_squares,minimize_scalar
ROOT=Path(__file__).resolve().parent
AU=1.495978707e11;mu=1.32712440018e20;T0=np.sqrt(AU**3/mu);V0=np.sqrt(mu/AU);A0=mu/AU**2;J0=V0**2/T0
P=1e9;m0=3e6;md=1e6;chi=2*P/m0/J0;kap=2*P*(1/md-1/m0)/J0
data=np.load(ROOT/'inputs/atlas_seeds.npz')
k_gain=-3.725e-6-4.91294688e-6
def fun(t,y):
 r=y[:2];v=y[2:4];pr=y[4:6];pv=y[6:8];R=np.linalg.norm(r)
 G=-np.eye(2)/R**3+3*np.outer(r,r)/R**5
 return np.r_[v,-r/R**3+pv/2,-G@pv,-pr,pv@pv/4]
def run(q,dense=False):
 return solve_ivp(fun,[0,np.exp(q[4])],np.r_[1.,0.,0.,1.,q[:4],0.],rtol=3e-10,atol=2e-12,method='DOP853',dense_output=dense)
def seed(rho,deg):
 i=np.argmin(abs(data['rho']-rho));j=np.argmin(abs(data['kappa']-kap));k=np.argmin(abs(data['theta']-np.deg2rad(deg)))
 if not np.isfinite(data['data'][i,j,k]).all():
  ii,jj,kk=np.where(np.isfinite(data['data'][...,0]))
  score=(np.log(data['rho'][ii]/rho))**2+(np.log(data['kappa'][jj]/kap))**2+((data['theta'][kk]-np.deg2rad(deg))/2)**2
  z=np.argmin(score);i,j,k=ii[z],jj[z],kk[z]
 lr,lvr,lvt,cm,ct,days=data['data'][i,j,k]
 pr=2*k_gain*T0/A0*np.array([lr,ct/AU+lvr/T0]);pv=2*k_gain/A0*np.array([lvr,lvt])
 return np.r_[pr,pv,np.log(days*86400/T0)]
out={'constants':dict(AU=AU,mu=mu,T0_days=T0/86400,V0=V0,J0=J0,chi=chi,kappa=kap),'cases':[]}
for rho,deg in [(5.2,80),(5.,273.1)]:
 th=np.deg2rad(deg);sf=np.r_[rho*np.cos(th),rho*np.sin(th),-np.sin(th)/np.sqrt(rho),np.cos(th)/np.sqrt(rho)]
 def residual(q):
  s=run(q);y=s.y[:,-1]
  return np.r_[(y[:2]-sf[:2])/5,y[2:4]-sf[2:4],(y[8]-kap)/kap]
 q=seed(rho,deg)
 opt=least_squares(residual,q,max_nfev=120,xtol=2e-12,ftol=2e-12,gtol=2e-12,diff_step=2e-7)
 if max(abs(residual(opt.x)))>1e-6:raise RuntimeError('endpoint solve failed')
 s=run(opt.x,True);h=s.t[-1];tt=np.linspace(0,h,40001);y=s.sol(tt);rr=np.linalg.norm(y[:2],axis=0);a=np.linalg.norm(y[6:8],axis=0)/2;nu=(chi+y[8])/a;ve=V0*nu/1000
 item=dict(rho=rho,theta_deg=deg,time_days=h*T0/86400,h=h,p0=opt.x[:4].tolist(),residual_max=float(max(abs(residual(opt.x)))),radius_min_AU=float(min(rr)),exhaust_min_kms=float(min(ve)),exhaust_max_kms=float(max(ve)),effort=float(y[8,-1]),band_pass=bool(min(ve)>=50 and max(ve)<=250),floor_pass=bool(min(rr)>=.1))
 # Dense sampling is diagnostic; a single interior violation rejects a continuous path.
 for key,ix in [('min_radius',np.argmin(rr)),('max_exhaust',np.argmax(ve)),('min_exhaust',np.argmin(ve))]:
  item[key+'_time_days']=float(tt[ix]*T0/86400)
 np.savez_compressed(ROOT/f'heliocentric_{deg:g}.npz',t=tt,y=y)
 out['cases'].append(item);print(json.dumps(item),flush=True)
 (ROOT/'heliocentric_results.json').write_text(json.dumps(out,indent=2))
