"""Independent endpoint/normalization audit of recoverable original candidates.

This certifies numerical residuals of reconstructions, not identity with every
historical table entry. All four historical tables receive a cell-by-cell
status ledger; unavailable trajectories have blank, explicitly unverified diagnostics.
"""
import json,csv,re
import numpy as np
from pathlib import Path
from scipy.optimize import least_squares,brentq,minimize_scalar
from polar_sensitivities import integrate
ROOT=Path(__file__).resolve().parent
co=json.loads((ROOT/'heliocentric_results.json').read_text())['constants'];kap=co['kappa']
rows=json.loads((ROOT/'jupiter_table_audit.json').read_text())
def solve(q,rho,theta):
 target=np.array([rho,theta,0,1/np.sqrt(rho),kap]);scale=np.array([max(1,rho),1,1,1,kap]);cache={}
 def ev(z):
  if 'q' not in cache or not np.array_equal(z,cache['q']):
   sol,y,D=integrate(z);cache.update(q=z.copy(),sol=sol,res=(y[[0,1,2,3,7]]-target)/scale,jac=D[[0,1,2,3,7]]/scale[:,None])
  return cache
 opt=least_squares(lambda z:ev(z)['res'],q,jac=lambda z:ev(z)['jac'],x_scale='jac',max_nfev=100,xtol=1e-11,ftol=1e-11,gtol=1e-11)
 assert max(abs(ev(opt.x)['res']))<2e-7,(rho,theta,opt.fun)
 return opt.x

def diagnostic(q,rho,theta,label):
 sol,y,D=integrate(np.array(q));h=np.exp(q[-1]);tt=np.unique(np.r_[np.linspace(0,h,8001),sol.t]);z=sol.sol(tt)[:8]
 def speed(t):
  s=sol.sol(t);return co['V0']*(co['chi']+s[7])*2/np.hypot(s[5],s[6])/1000
 # Refine sampled local extrema and all radial turning points.
 ve=np.array([speed(t) for t in tt]);vs=[ve[0],ve[-1]];rc=[0,h]
 for i in np.where(z[2,1:]*z[2,:-1]<0)[0]:rc.append(brentq(lambda t:sol.sol(t)[2],tt[i],tt[i+1],xtol=1e-14))
 for sign in [1,-1]:
  ix=np.where((sign*ve[1:-1]<sign*ve[:-2])&(sign*ve[1:-1]<sign*ve[2:]))[0]+1
  for i in ix:
   o=minimize_scalar(lambda t:sign*speed(t),bounds=(tt[i-1],tt[i+1]),method='bounded',options={'xatol':1e-14});vs.append(speed(o.x))
 E=z[4]*z[2]+q[3]*z[3]/z[0]+z[5]*(z[3]**2/z[0]-1/z[0]**2)-z[6]*z[2]*z[3]/z[0]+(z[5]**2+z[6]**2)/4
 E0=q[3]+(q[1]**2+q[2]**2)/4
 res=y[[0,1,2,3]]-[rho,theta,0,1/np.sqrt(rho)]
 record=dict(label=label,rho=rho,angle_deg=np.rad2deg(theta),time_days=h*co['T0_days'],radius_min_AU=min(float(sol.sol(t)[0]) for t in rc),exhaust_min_kms=min(vs),exhaust_max_kms=max(vs),normalized_endpoint_residual=max(abs(res)),effort_residual=y[7]-kap,final_mass_kg=3e6*co['chi']/(co['chi']+y[7]),effort_H_initial=E0,normal_fixed_endpoint_scaling_positive=bool(E0>0),normal_time_H_max_abs=max(abs(E/E0-1)),q=list(q))
 assert sol.success
 assert record['normal_fixed_endpoint_scaling_positive']
 assert record['normalized_endpoint_residual']<2e-7
 assert abs(record['effort_residual'])<1e-6
 assert record['normal_time_H_max_abs']<5e-6
 return record
if __name__=='__main__':
 records=[]
 for row in rows:
  rec=diagnostic(row['q'],5.2,np.deg2rad(row['angle_deg']),f"5.2 AU final-angle row {row['angle_deg']}")
  records.append(rec)
  if len(records)%10==0: print('Audited',len(records),flush=True)
 (ROOT/'jupiter_normalization_audit.json').write_text(json.dumps(records,indent=2))
 q=np.array(next(r['q'] for r in rows if r['angle_deg']==80))
 for rho in np.linspace(5.2,1.5,25)[1:]: q=solve(q,rho,np.deg2rad(80))
 for deg in np.linspace(80,34.7,25)[1:]:q=solve(q,1.5,np.deg2rad(deg))
 figs=[diagnostic(q,1.5,np.deg2rad(34.7),'Figure: 1.5 AU, 34.7 degrees')]
 figcases=json.loads((ROOT/'heliocentric_results.json').read_text())['cases']
 for case in figcases[1:]:
  p=case['p0'];q=np.r_[p[0],p[2],p[3],p[1]-p[2],np.log(case['h'])]
  if 'polar_p0' in case:q[:3]=case['polar_p0'];q[3]=case['polar_angular_costate']
  figs.append(diagnostic(q,case['rho'],np.deg2rad(case['theta_deg']),f"Figure: {case['rho']} AU, {case['theta_deg']} degrees"))
 (ROOT/'figure_audit.json').write_text(json.dumps(figs,indent=2))
 print(json.dumps(figs,indent=2),flush=True)
