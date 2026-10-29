import numpy as np,math,json
from pathlib import Path
def rot(t):return np.array([[math.cos(t),-math.sin(t)],[math.sin(t),math.cos(t)]])
J=np.array([[0.,-1.],[1.,0.]])
def limit(w,mu,rp):
 k=rp/mu;e=1+k*(w@w);return 2*math.asin(1/e),-4*k*w/(e*math.sqrt((e-1)*(e+1)))
w=np.array([1.,0.]);vp=np.array([0.,1.]);ap=np.array([-1.,0.]);t,dg=limit(w,.01,.01);wp=rot(t)@w
M=rot(t)+np.outer(J@wp,dg);pvp=J@wp;pvm=M.T@pvp;prm=np.array([-1/3,0]);prp=np.zeros(2)
Hm=prm@(vp+w)+pvm@ap+pvm@pvm/4;Hp=pvp@ap+pvp@pvp/4
gt=Hp-Hm+(prm-prp)@vp+(pvm-pvp)@ap
err=0
for j in range(2):
 step=np.eye(2)[j]*1e-6
 f=lambda q:rot(limit(q,.01,.01)[0])@q
 err=max(err,float(np.max(np.abs((f(w+step)-f(w-step))/2e-6-M[:,j]))))
assert err<1e-8 and abs(gt)<1e-12
t2=.3;wp2=rot(t2)@w;pvp2=2*wp2;pvm2=2*w
Hm2=pvm2@ap+pvm2@pvm2/4;Hp2=pvp2@ap+pvp2@pvp2/4
mu3=3e-6;rp3=5e-5;w3=np.array([.5,0]);d3,_=limit(w3,mu3,rp3)
out={'case1':{'mu_planet':.01,'rmin':.01,'planet_r':[1,0],'planet_v':vp.tolist(),'incoming_excess':w.tolist(),'turn':t,'outgoing_excess':wp.tolist(),'outgoing_velocity':(vp+wp).tolist(),'delta_energy':float(vp@(wp-w)),'fixed_radius_jacobian':M.tolist(),'p_v_minus':pvm.tolist(),'p_v_plus':pvp.tolist(),'p_r_minus':prm.tolist(),'p_r_plus':prp.tolist(),'H_minus':float(Hm),'H_plus':float(Hp),'upper_turn_multiplier':1,'epoch_residual':float(gt),'jacobian_fd_error':err},
'case2':{'turn':t2,'incoming_excess':w.tolist(),'outgoing_excess':wp2.tolist(),'p_v_minus':pvm2.tolist(),'p_v_plus':pvp2.tolist(),'p_r_both':[0,0],'H_minus':float(Hm2),'H_plus':float(Hp2),'turn_multipliers':[0,0],'epoch_residual':float(Hp2-Hm2+(pvm2-pvp2)@ap),'radius_required':float(.01*(1/math.sin(t2/2)-1))},
'case3':{'mu_planet':mu3,'rmin':rp3,'excess_speed':.5,'requested_turn':.6,'max_turn':d3,'required_radius':mu3/.25*(1/math.sin(.3)-1),'max_kick':1/(1+rp3*.25/mu3),'status':'reject: turn requires periapsis below minimum'}}
Path(__file__).with_name('test_cases.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
