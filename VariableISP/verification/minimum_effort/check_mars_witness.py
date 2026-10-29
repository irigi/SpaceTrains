"""Independent floating-point dynamics and quadrature check of the Hermite witness.

This is an additional check, not the interval certificate. NumPy/SciPy required.
"""
from pathlib import Path
import json
import numpy as np
from scipy.integrate import quad, solve_ivp

base=Path(__file__).resolve().parent
d=json.loads((base/'mars_witness_knots.json').read_text())
AU=149597870700.;mu=1.32712440018e20;T0=np.sqrt(AU**3/mu)
V0=np.sqrt(mu/AU);J0=V0**2/T0;chi=2e9/3e6/J0
H=d['duration_days']*86400/T0;R=float(d['target_radius_AU'])
q=float(d['target_half_angle_tangent']);cs=(1-q*q)/(1+q*q);sn=2*q/(1+q*q)
t=np.array([float(k['fraction']) for k in d['knots']])*H;t[0]=0;t[-1]=H
x=np.array([[float(v) for v in k['position']] for k in d['knots']])
v=np.array([[float(v) for v in k['velocity']] for k in d['knots']])
x[0]=[1,0];v[0]=[0,1];x[-1]=R*np.array([cs,sn]);v[-1]=np.array([-sn,cs])/np.sqrt(R)
state=np.r_[x[0],v[0],3e6];effort=0.;max_knot_error=0.
for j,dt in enumerate(np.diff(t)):
    c=np.array([x[j],dt*v[j],3*(x[j+1]-x[j])-dt*(2*v[j]+v[j+1]),2*(x[j]-x[j+1])+dt*(v[j]+v[j+1])])
    def reference(s):
        r=c[0]+s*(c[1]+s*(c[2]+s*c[3]))
        acc=(2*c[2]+6*s*c[3])/dt**2
        u=acc+r/np.linalg.norm(r)**3
        return r,u
    effort+=quad(lambda s:np.dot(reference(s)[1],reference(s)[1])*dt,0,1,epsabs=1e-12,epsrel=1e-12)[0]
    def rhs(tau,y):
        u=reference(tau/dt)[1];r=y[:2];vel=y[2:4];m=y[4]
        return np.r_[vel,-r/np.linalg.norm(r)**3+u,-m*m/(2e9)*J0*np.dot(u,u)]
    sol=solve_ivp(rhs,[0,dt],state,method='DOP853',rtol=2e-12,atol=np.array([1e-13]*4+[1e-6]))
    assert sol.success
    state=sol.y[:,-1]
    max_knot_error=max(max_knot_error,np.max(np.abs(state[:4]-np.r_[x[j+1],v[j+1]])))
mass_formula=3e6*chi/(chi+effort)
certificate=json.loads((base/'mars_witness_certificate.json').read_text())
assert float(certificate['effort_enclosure'][0])<effort<float(certificate['effort_enclosure'][1])
assert max_knot_error<1e-9 and abs(mass_formula-state[4])<.01
result={'passed':True,'quadrature_effort':effort,'mass_formula_kg':mass_formula,'integrated_mass_kg':state[4],'maximum_kinematic_knot_error':max_knot_error,'propellant_kg':3e6-mass_formula,'endpoint_position':x[-1].tolist(),'endpoint_velocity':v[-1].tolist()}
(base/'mars_witness_crosscheck.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
