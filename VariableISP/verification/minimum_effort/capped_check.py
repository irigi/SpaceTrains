import numpy as np,json
from scipy.integrate import solve_ivp
from scipy.optimize import least_squares,minimize
from pathlib import Path
P=.5;V=2.;md=.5

def flow(t,y,tz):
 x,v,m,lm=y;l=tz-t;C=lm*m*m
 if C<=0 or m<=0:return [v,0,0,0]
 a=max(P*abs(l)/C,2*P/(m*V));gain=abs(l)*a-C*a*a/(2*P)
 if gain<=0:a=0
 return [v,np.sign(l)*a,-m*m*a*a/(2*P),abs(l)*a/m]
def run(q,dense=False):
 tz,lm,T=np.exp(q)
 return solve_ivp(lambda t,y:flow(t,y,tz),[0,T],[0,0,1,lm],rtol=2e-9,atol=2e-11,max_step=T/150,dense_output=dense)
def res(q):
 s=run(q);return s.y[:3,-1]-[1,0,md]
r=least_squares(res,np.log([1.2,.8,2.5]),diff_step=2e-4,xtol=1e-11,ftol=1e-11,gtol=1e-11,max_nfev=100)
s=run(r.x,True);tz,lm,T=np.exp(r.x)
ts=np.linspace(0,T,20001);ys=s.sol(ts);acc=np.array([flow(t,y,tz)[1] for t,y in zip(ts,ys.T)])
inds=np.where(abs(acc)>1e-7)[0];aend=ts[np.where((ts<tz)&(abs(acc)>0))[0][-1]];bstart=ts[np.where((ts>tz)&(abs(acc)>0))[0][0]]
out={'P':P,'Vmax':V,'m0':1,'md':md,'distance':1,'shooting_T':T,'primer_zero':tz,'lambda_m0':lm,'shooting_residual':res(r.x).tolist(),'coast_edges_sampled':[aend,bstart],'transcription':[]}
for N in [12,24,48]:
 dp=aend/N;dm=(T-bstart)/N;co=bstart-aend
 ap=np.interp((np.arange(N)+.5)*dp,ts,acc);am=-np.interp(bstart+(np.arange(N)+.5)*dm,ts,acc)
 q=np.r_[ap,am,dp,dm,co]
 def calc(q):
  aa=np.r_[q[:N],0,-q[N:2*N]];dt=np.r_[np.full(N,q[-3]),q[-1],np.full(N,q[-2])]
  w=1.;x=v=0.; margins=[]
  for a,d in zip(aa,dt):
   w+=a*a*d;x+=v*d+.5*a*d*d;v+=a*d
   if a: margins.append(abs(a)-w/V)
  return np.array([x-1,v,w-2]),np.array(margins)
 opt=minimize(lambda q:N*(q[-3]+q[-2])+q[-1],q,method='SLSQP',bounds=[(1e-5,None)]*(2*N+2)+[(0,None)],constraints=[{'type':'eq','fun':lambda q:calc(q)[0]},{'type':'ineq','fun':lambda q:calc(q)[1]}],options={'ftol':2e-11,'maxiter':600})
 out['transcription'].append({'N_burn_cells':N,'success':bool(opt.success),'T':opt.fun,'endpoint_max':float(max(abs(calc(opt.x)[0]))),'band_margin':float(min(calc(opt.x)[1]))})
Path(__file__).with_name('capped_results.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
