"""Continue all 73 final-angle rows of the original 5.2 AU table."""
import numpy as np,json,csv,re
from pathlib import Path
from scipy.optimize import least_squares,brentq,minimize_scalar
from polar_sensitivities import integrate
ROOT=Path(__file__).resolve().parent;out=json.loads((ROOT/'heliocentric_results.json').read_text());co=out['constants'];case=out['cases'][0];p=case['p0'];kap=co['kappa']
q0=np.array([p[0],p[2],p[3],p[1]-p[2],np.log(case['h'])]);records=[]
for angles in [range(80,361,5),range(75,-1,-5)]:
 q=q0.copy()
 for deg in angles:
  target=np.array([5.2,np.deg2rad(deg),0,1/np.sqrt(5.2),kap]);scale=np.array([5,1,1,1,kap]);cache={}
  def evaluate(z):
   if 'q' not in cache or not np.array_equal(cache['q'],z):
    sol,y,D=integrate(z);cache.update(q=z.copy(),sol=sol,res=(y[[0,1,2,3,7]]-target)/scale,jac=D[[0,1,2,3,7]]/scale[:,None])
   return cache
  opt=least_squares(lambda z:evaluate(z)['res'],q,jac=lambda z:evaluate(z)['jac'],x_scale='jac',max_nfev=70,xtol=2e-11,ftol=2e-11,gtol=2e-11)
  q=opt.x;result=evaluate(q);err=float(max(abs(result['res'])))
  if err>2e-6:raise RuntimeError((deg,err))
  sol=result['sol'];h=np.exp(q[-1]);tt=np.unique(np.r_[np.linspace(0,h,4001),sol.t]);y=sol.sol(tt)[:8];r=y[0];ve=co['V0']*(co['chi']+y[7])/np.hypot(y[5],y[6])*2/1000
  candidates=[0,h]
  for ix in np.where(y[2,1:]*y[2,:-1]<0)[0]:candidates.append(brentq(lambda t:sol.sol(t)[2],tt[ix],tt[ix+1]))
  rmin=min(float(sol.sol(t)[0]) for t in candidates)
  row=dict(angle_deg=deg,time_days=float(h*co['T0_days']),radius_min_AU=rmin,exhaust_min_sample_kms=float(min(ve)),exhaust_max_sample_kms=float(max(ve)),initial_exhaust_kms=float(ve[0]),band_rejected=bool(min(ve)<50 or max(ve)>250),floor_rejected=bool(rmin<.1),scaled_endpoint_residual=err,q=q.tolist())
  records.append(row)
  (ROOT/'jupiter_table_audit.json').write_text(json.dumps(sorted(records,key=lambda x:x['angle_deg']),indent=2))
  print(deg,round(row['time_days'],5),round(rmin,6),row['band_rejected'],flush=True)
with (ROOT/'jupiter_table_audit.csv').open('w') as f:
 writer=csv.DictWriter(f,fieldnames=[k for k in records[0] if k!='q']);writer.writeheader()
 for row in sorted(records,key=lambda x:x['angle_deg']):writer.writerow({k:v for k,v in row.items() if k!='q'})
