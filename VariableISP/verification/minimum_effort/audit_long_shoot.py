"""Polar shooting with analytic sensitivities preserves the >360 degree winding in the original figure."""
import numpy as np,json
from scipy.integrate import solve_ivp,solve_bvp
from scipy.optimize import least_squares,minimize_scalar,brentq
from polar_sensitivities import integrate
from pathlib import Path
ROOT=Path(__file__).resolve().parent;out=json.loads((ROOT/'heliocentric_results.json').read_text());co=out['constants'];a=np.load(ROOT/'inputs/atlas_seeds.npz')
i=np.argmin(abs(a['rho']-4.8255));j=np.argmin(abs(a['kappa']-6.54));k=np.argmin(abs(a['theta']-np.deg2rad(382.7)))
lr,lvr,lvt,_,ct,days=a['data'][i,j,k];T0=co['T0_days']*86400;AU=co['AU'];A0=co['mu']/AU**2;gain=-3.725e-6-4.91294688e-6
pr=2*gain*T0/A0*lr;pvr=2*gain/A0*lvr;pvt=2*gain/A0*lvt;pt=2*gain*T0/A0/AU*ct;h=days/co['T0_days']
def flow(y,pt):
 r,th,vr,vt,pr,pvr,pvt,I=y
 return np.array([vr,vt/r,vt*vt/r-1/r**2+pvr/2,-vr*vt/r+pvt/2,pt*vt/r**2+pvr*vt**2/r**2-2*pvr/r**3-pvt*vr*vt/r**2,-pr+pvt*vt/r,-pt/r-2*pvr*vt/r+pvt*vr/r,(pvr*pvr+pvt*pvt)/4])
s=solve_ivp(lambda t,y:flow(y,pt),[0,h],[1,0,0,1,pr,pvr,pvt,0],rtol=1e-10,atol=1e-12,method='DOP853',dense_output=True)
mesh=np.unique(np.r_[np.linspace(0,1,600),s.t/h]);y=s.sol(mesh*h);par=np.array([pt,np.log(h)])
print('initial',s.y[:,-1],flush=True)
q=np.r_[pr,pvr,pvt,pt,np.log(h)]
check=ROOT/'long_shoot_checkpoint.npz'
fractions=np.linspace(0,1,12)
if check.exists():
 saved=np.load(check);q=saved['q'];fractions=fractions[fractions>=float(saved['frac'])]
for frac in fractions:
 rho=(1-frac)*a['rho'][i]+frac*5.2;theta=(1-frac)*a['theta'][k]+frac*np.deg2rad(385.2);kap=(1-frac)*a['kappa'][j]+frac*co['kappa']
 def run(q):return solve_ivp(lambda t,y:flow(y,q[3]),[0,np.exp(q[4])],np.r_[1,0,0,1,q[:3],0],rtol=2e-11,atol=2e-12,method='DOP853',dense_output=True)
 cache={}
 def evaluate(qq):
  if 'q' not in cache or not np.array_equal(cache['q'],qq):
   ss,z,D=integrate(qq);scale=np.array([5,1,1,1,kap]);ix=[0,1,2,3,7]
   cache.update(q=qq.copy(),res=(z[ix]-[rho,theta,0,1/np.sqrt(rho),kap])/scale,jac=D[ix]/scale[:,None])
  return cache
 def resid(qq):return evaluate(qq)['res']
 opt=least_squares(resid,q,jac=lambda qq:evaluate(qq)['jac'],max_nfev=100,xtol=1e-12,ftol=1e-12,gtol=1e-12,x_scale='jac')
 print('step',frac,max(abs(resid(opt.x))),np.exp(opt.x[-1]),flush=True)
 if max(abs(resid(opt.x)))>1e-5:raise RuntimeError('shooting failed')
 q=opt.x
 np.savez_compressed(ROOT/'long_shoot_checkpoint.npz',q=q,frac=frac)
integrated_solution,_,_=integrate(q)
class StateSolution:
 def sol(self,t):return integrated_solution.sol(t)[:8]
s=StateSolution();h=np.exp(q[-1]);par=np.array([q[3],q[-1]])
class Dense:
 rms_residuals=[0.]
 def sol(self,t):return s.sol(t*h)
ss=Dense()
def bc(ya,yb,p):return np.r_[ya[:4]-[1,0,0,1],ya[7],yb[:4]-[rho,theta,0,1/np.sqrt(rho)],yb[7]-kap]
h=np.exp(par[1]);tt=np.linspace(0,h,40001);yp=ss.sol(tt/h);r,th,vr,vt,pr,pvr,pvt,I=yp
er=np.array([np.cos(th),np.sin(th)]);et=np.array([-np.sin(th),np.cos(th)])
pv=pvr*er+pvt*et;pc=pr*er+(par[0]+pvr*vt-pvt*vr)/r*et
y=np.vstack([r*er,vr*er+vt*et,pc,pv,I]);ve=co['V0']*(co['chi']+I)/(np.hypot(pvr,pvt)/2)/1000
item=dict(rho=5.2,theta_deg=385.2,time_days=h*co['T0_days'],h=h,p0=y[4:8,0].tolist(),polar_p0=yp[4:7,0].tolist(),polar_angular_costate=float(par[0]),residual_max=float(max(abs(bc(yp[:,0],yp[:,-1],par)))),shooting_residual=float(max(abs(resid(q)))),radius_min_AU=float(min(r)),exhaust_min_kms=float(min(ve)),exhaust_max_kms=float(max(ve)),effort=float(I[-1]),band_pass=bool(min(ve)>=50 and max(ve)<=250),floor_pass=bool(min(r)>=.1))

# Resolve the very narrow perihelion rather than relying on uniform samples.
imin=int(np.argmin(r));lo=tt[max(0,imin-2)];hi=tt[min(len(tt)-1,imin+2)]
tperi=brentq(lambda t:s.sol(t)[2],lo,hi,xtol=5e-15)
item['radius_min_AU']=float(s.sol(tperi)[0]);item['perihelion_time_days']=float(tperi*co['T0_days'])
def exhaust(t):
 yy=s.sol(t);return co['V0']*(co['chi']+yy[7])/(np.hypot(yy[5],yy[6])/2)/1000
for sign,key in [(1,'exhaust_min_kms'),(-1,'exhaust_max_kms')]:
 arr=sign*ve;ids=np.where((arr[1:-1]<arr[:-2])&(arr[1:-1]<arr[2:]))[0]+1
 vals=[exhaust(0),exhaust(h)]
 for ix in ids:
  oo=minimize_scalar(lambda t:sign*exhaust(t),bounds=(tt[ix-1],tt[ix+1]),method='bounded',options={'xatol':1e-14})
  vals.append(exhaust(oo.x))
 vals.append(exhaust(tperi))
 item[key]=float(min(vals) if sign==1 else max(vals))

out['cases']=out['cases'][:2]+[item];(ROOT/'heliocentric_results.json').write_text(json.dumps(out,indent=2));np.savez_compressed(ROOT/'heliocentric_385.2.npz',t=tt,y=y,polar=yp);print(json.dumps(item),flush=True)
