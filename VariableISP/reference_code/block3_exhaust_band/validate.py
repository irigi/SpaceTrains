import json, math
from pathlib import Path
from scipy.integrate import solve_ivp
P=.5; V=1.; m0=1.; lm0=.4; L0=.6; nu=.2; t=.1
sol=solve_ivp(lambda t,y:[-1, (.6+.2*t)/y[0]**2], [0,t],[m0,lm0],rtol=1e-12,atol=1e-14)
m=.9; lm=.4+.8*(1/m-1)+.2*math.log(m)
assert abs(sol.y[1,-1]-lm)<1e-12
C=m*m*lm; Cdot=(.6+.2*t)-2*C/m
z=(.6+.2*t)*m/(2*C)
w0=.2; a=.05; vmax=2; h=1
wf=w0*math.exp(a*h/vmax)
out={'case1':{'w':.2,'vmin':.4,'vmax':2,'C':1,'s':[.05,.15,.4,2], 'a':[0,.1,.2,.5], 'Cdot':[0,-.025,0,2.5]},
'case2':{'P':P,'V':V,'m0':m0,'lambda_m0':lm0,'lambda_v0':L0,'lambda_v_slope':nu,'t':t,'m':m,'lambda_m':lm,'C_initial':.4,'C_final':C,'Cdot_final':Cdot,'z_final':z,'ode_error':abs(sol.y[1,-1]-lm)},
'case3':{'w0':w0,'a':a,'vmin':.4,'vmax':vmax,'h':h,'ideal_J':a*a*h,'ideal_wf':w0+a*a*h,'throttled_wf':wf,'throttled_resource_effort':wf-w0,'extra_resource':wf-w0-a*a*h,'throttled_fuel_fraction':1-w0/wf,'ideal_fuel_fraction':1-w0/(w0+a*a*h),'initial_duty':a*vmax/w0,'final_duty':a*vmax/wf},
'physical':{'useful_power_per_initial_mass_W_per_kg':540,'initial_min_acceleration_m_s2':1080/250000,'initial_max_acceleration_m_s2':1080/50000}}
Path(__file__).with_name('test_cases.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
