"""Exact shaped heliocentric witness and conservative analytic bounds."""
import numpy as np,json
from scipy.integrate import quad
import math
class MP:
 mpf=staticmethod(float);sqrt=staticmethod(math.sqrt);cos=staticmethod(math.cos);sin=staticmethod(math.sin);cosh=staticmethod(math.cosh);sinh=staticmethod(math.sinh);matrix=staticmethod(np.array);norm=staticmethod(np.linalg.norm)
 @staticmethod
 def quad(f,limits):return quad(f,*limits,epsabs=1e-14)[0]
mp=MP()
from pathlib import Path

mu=mp.mpf('1.32712440018e20');AU=mp.mpf('1.495978707e11');T0=mp.sqrt(AU**3/mu);V0=mp.sqrt(mu/AU);J0=V0**2/T0
P=mp.mpf('1e9');m0=mp.mpf('3e6');chi=2*P/m0/J0
c=mp.mpf('.4');H=mp.mpf('.05');kap=4*c*c*H+c*c*H**3/3
J=4*c*c*H+mp.quad(lambda t:c*c*t*t/(1+c*t*t)**3,[0,H])
r=1+c*H*H;theta=H/mp.sqrt(r);er=mp.matrix([mp.cos(theta),mp.sin(theta)]);et=mp.matrix([-mp.sin(theta),mp.cos(theta)])
rf=r*er;vf=2*c*H*er+et/mp.sqrt(r)
rc=mp.matrix([mp.cos(H),mp.sin(H)]);vc=mp.matrix([-mp.sin(H),mp.cos(H)])
dr=mp.norm(rf-rc);dv=mp.norm(vf-vc)
assert dr<mp.mpf('.00101') and dv>mp.mpf('.0399') and dv<mp.mpf('.041')
K=2/mp.mpf('.9')**3;om=mp.sqrt(K)
assert mp.cosh(om*H)**2<mp.mpf('1.007')
# Backward ballistic target versus unit coast on the final .001 interval.
eps=mp.mpf('.001');dmax=dr*mp.cosh(om*eps)+dv*mp.sinh(om*eps)/om
assert dmax<mp.mpf('.0011')
assert K*dmax*eps<mp.mpf('.0000031')
LB=(mp.mpf('.0399')-mp.mpf('.0000031'))**2/(mp.mpf('.049')*mp.mpf('1.007'))
assert LB>kap
L=mp.mpf('.0399')**2/(H*mp.mpf('1.007'));assert L>mp.mpf('.0316')
kr2=(mp.sinh(2*om*H)/(4*om)-H/2)/om**2
assert mp.sqrt(kap*kr2)<mp.mpf('.0012')<mp.mpf('.1')
amin=2*c;amax=mp.sqrt(4*c*c+c*c*H*H)
vemn=V0*chi/amax/1000;vemx=V0*(chi+kap)/amin/1000
assert vemn>50 and vemx<250
out=dict(chi=chi,kappa=kap,J=J,L_at_H=mp.mpf('.0316'),U_at_H=kap,h_lower=mp.mpf('.049'),h_upper=H,time_lower_days=mp.mpf('.049')*T0/86400,time_upper_days=H*T0/86400,propellant_kg=m0-m0*chi/(chi+J),dry_mass_kg=m0*chi/(chi+kap),exhaust_lower_kms=vemn,exhaust_upper_kms=vemx,radial_min_AU=1,delta_r_final=dr,delta_v_final=dv,earlier_lower_bound=LB)
out={k:float(v) for k,v in out.items()};out['rf']=[float(v) for v in rf];out['vf']=[float(v) for v in vf]
Path(__file__).with_name('worked_heliocentric_results.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
