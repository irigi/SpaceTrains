import json,math
from pathlib import Path
import numpy as np
from scipy.integrate import quad,solve_ivp
from escape_reference import *
HERE=Path(__file__).parent
cross=lambda x,y:x[0]*y[1]-x[1]*y[0]

# 1: analytic optimum of the phase-free MONOTONE AVERAGED model.
D=.5;chi=.01;nlo=10.;nhi=50.
rows=[]
for h in [480.,490.,1000.,2480.,3000.]:
    row=minimum_effort(D,h,chi,nlo,nhi);row['duration']=h
    if 'effort' in row:
        J=row['effort'];row['mass_fraction']=chi/(chi+J)
        assert J>=D*D/h*(1-1e-9)
        assert J>=fixed_exhaust(D,chi,nhi)['effort']*(1-1e-10)
        if row['coast_time']==0:
            k=row['k'];W=chi+J
            z=lambda w:np.clip(k,nlo/w,nhi/w)
            dq=quad(lambda w:z(w),chi,W,epsabs=1e-13,points=[p[2] for p in row['segments'][:-1]])[0]
            tq=quad(lambda w:z(w)**2,chi,W,epsabs=1e-10,points=[p[2] for p in row['segments'][:-1]])[0]
            assert abs(dq-D)<1e-10 and abs(tq-h)<2e-6
            def f(t,y):
                a=1/z(y[1]);return [a,a*a]
            def ev(t,y):return y[0]-D
            ev.terminal=True;ev.direction=1
            sol=solve_ivp(f,[0,h*1.01],[0,chi],events=ev,rtol=2e-11,atol=2e-13,method='DOP853',max_step=h/120)
            assert len(sol.t_events[0])==1
            row['ode_time_error']=float(abs(sol.t_events[0][0]-h))
            row['ode_effort_error']=float(abs(sol.y_events[0][0,1]-chi-J))
            assert row['ode_time_error']<2e-5 and row['ode_effort_error']<1e-10
    rows.append(row)
assert rows[0]['status'].startswith('infeasible')
assert rows[1]['segments'][0][0]=='min_exhaust'
assert abs(rows[2]['effort']-.00025)<1e-13
assert rows[2]['segments'][0][0]=='constant_acceleration'
assert rows[3]['segments'][-1][0]=='max_exhaust'
assert rows[4]['coast_time']>0
times=[fastest_at_effort(D,chi,J,nlo,nhi)['time'] for J in np.linspace(fixed_exhaust(D,chi,nhi)['effort'],fixed_exhaust(D,chi,nlo)['effort'],30)]
assert np.all(np.diff(times)<0)
W=chi+rows[4]['effort']
phase_burn=quad(lambda w:(1-nhi*math.log(w/chi))**3*nhi*nhi/(w*w),chi,W,epsabs=1e-9)[0]
slack=rows[4]['coast_time']
case1=dict(model='Globally solved within phase-free monotone averaged model ONLY',
           ri=1.,rf=4.,D=D,chi=chi,nu_min=nlo,nu_max=nhi,samples=rows,
           constant_acceleration_phase=phase_constant_accel(1.,.5,1000.),
           coast_plateau_phase_interval=[phase_burn+.5**3*slack,phase_burn+slack],
           budget_time=minimum_time(D,.0002,chi,nlo,nhi))

# 2: exact, explicitly constructed escape and reflected reverse capture.
T=10.;R=4.;floor=.8;chi2=.9;nlo2=10.;nhi2=50.
U=quad(lambda t:float(exact_shape(t)[2]@exact_shape(t)[2]),0,T,epsabs=1e-14,epsrel=1e-13)[0]
x0,v0,_=exact_shape(0);xf,vf,_=exact_shape(T)
Uclosed=651/16000+math.pi*math.sqrt(3)/240
assert abs(U-Uclosed)<1e-14
def exact_rhs(t,y):
    x,v,u=exact_shape(t)
    return np.r_[y[2:4],gravity(y[:2])+u,u@u,y[2:4]@u,cross(y[:2],u)]
y0=np.r_[x0,v0,0.,0.,0.]
sol=solve_ivp(exact_rhs,[0,T],y0,method='DOP853',rtol=2e-12,atol=1e-13,dense_output=True)
np.testing.assert_allclose(sol.y[:4,-1],np.r_[xf,vf],atol=2e-9,rtol=0)
E=lambda x,v:v@v/2-1/np.linalg.norm(x)
assert abs(E(xf,vf)-E(x0,v0)-sol.y[5,-1])<2e-10
assert abs(cross(xf,vf)-cross(x0,v0)-sol.y[6,-1])<2e-10
reflect=np.diag([1.,-1.])
def cap_rhs(t,y):
    u=reflect@exact_shape(T-t)[2]
    return np.r_[y[2:4],gravity(y[:2])+u,u@u]
cap=solve_ivp(cap_rhs,[0,T],np.r_[reflect@xf,-reflect@vf,0.],method='DOP853',rtol=2e-12,atol=1e-13,dense_output=True)
np.testing.assert_allclose(cap.y[:4,-1],np.r_[reflect@x0,-reflect@v0],atol=2e-9,rtol=0)
assert abs(cap.y[4,-1]-U)<2e-12
# Analytic global band enclosure: 0.06 <= |u| <= sqrt(.06^2+(1/15)^2).
amin=.06;amax=math.sqrt(.06**2+(1/15)**2)
assert (chi2+U)/nhi2<amin and chi2/nlo2>amax
DW=abs(math.sqrt(2*E(xf,vf)+2/floor)-math.sqrt(2*E(x0,v0)+2/floor))
Dj=abs(cross(xf,vf)-cross(x0,v0))/R
Dnecessary=max(DW,Dj)
rigorous_formula_lower=max(Dnecessary**2/T,chi2*math.expm1(Dnecessary/nhi2))
assert rigorous_formula_lower<U
case2=dict(model='Exact feasible shaped path, NOT an extremal or optimum',T=T,soi_radius=R,
           planet_floor=floor,budget=.08,chi=chi2,nu_min=nlo2,nu_max=nhi2,
           initial_state=np.r_[x0,v0].tolist(),exit_state=np.r_[xf,vf].tolist(),
           exit_angle=5.,exit_radial_speed=.6,exit_energy=E(xf,vf),v_infinity=math.sqrt(2*E(xf,vf)),
           effort=U,effort_closed_form=Uclosed,mass_fraction=chi2/(chi2+U),amin_analytic=amin,amax_analytic=amax,
           band_lower_margin=amin-(chi2+U)/nhi2,band_upper_margin=chi2/nlo2-amax,
           work=sol.y[5,-1],angular_change=sol.y[6,-1],
           necessary_effort_lower=rigorous_formula_lower,
           forward_endpoint_error=float(np.max(np.abs(sol.y[:4,-1]-np.r_[xf,vf]))),
           capture_endpoint_error=float(np.max(np.abs(cap.y[:4,-1]-np.r_[x0,v0]))),
           capture_initial_state=np.r_[reflect@xf,-reflect@vf].tolist())

# 3: SOI chart gradients checked by finite differences of independent endpoint actions.
Rp=np.array([10.,0.]);Vp=np.array([0.,math.sqrt(.1)]);Ap=np.array([-.01,0.])
x=np.array([0.,1.]);vrel=np.array([.5,-2.]);X=Rp+x;V=Vp+vrel
prm=np.array([.1,-.2]);pv=np.array([.3,.4])
Gsun=gravity(X);Gplanet=Ap+gravity(x,.01)
z,prp,dH=soi_jump(x,vrel,Vp,Gplanet-Gsun,pv,prm)
Hm=prm@V+pv@Gsun+pv@pv/4
Hp=prp@V+pv@Gplanet+pv@pv/4
assert abs(Hp-Hm-dH)<1e-14
analytic=interface_gradient(x,vrel,Vp,Ap,prm,pv,Hm,prp,pv,Hp)
np.testing.assert_allclose(analytic,0,atol=1e-14)
# Perturb outgoing costates too, to test nonzero gradients (not just cancellation).
prb=prp+np.array([.07,-.02]);pvb=pv+np.array([-.03,.05]);Hb=Hp+.11
ang=math.pi/2;vr=-2.;vt=-.5

def action(q):
    th,rr,tt,dt=q;er=np.array([math.cos(th),math.sin(th)]);et=np.array([-er[1],er[0]])
    pos=Rp+Vp*dt+.5*Ap*dt*dt+er
    vel=Vp+Ap*dt+rr*er+tt*et
    return (prm-prb)@pos+(pv-pvb)@vel+(Hb-Hm)*dt
q0=np.array([ang,vr,vt,0.]);eps=1e-5
fd=np.array([(action(q0+eps*np.eye(4)[i])-action(q0-eps*np.eye(4)[i]))/(2*eps) for i in range(4)])
expected=interface_gradient(x,vrel,Vp,Ap,prm,pv,Hm,prb,pvb,Hb)
np.testing.assert_allclose(fd,expected,atol=2e-10,rtol=0)
sc=scale_factors(.001,3e-6)
# Covector pairing invariant under local -> common normalized conversion.
dx=np.array([.02,-.03]);dv=np.array([.04,.01]);plr=np.array([.7,-.2]);plv=np.array([-.3,.5])
common=(sc['effort']/sc['length']*plr)@(sc['length']*dx)+(sc['effort']/sc['velocity']*plv)@(sc['velocity']*dv)
assert abs(common-sc['effort']*(plr@dx+plv@dv))<1e-14
case3=dict(model='Common-coordinate SOI junction and derivative fixture',multiplier=z,
           p_position_after=prp.tolist(),hamiltonian_jump=dH,stationarity_residual=analytic.tolist(),
           nonstationary_chart_gradient=expected.tolist(),finite_difference_max_error=float(np.max(np.abs(fd-expected))),
           scale_test=sc)

# Physical illustration; chosen parking/SOI radii, not an optimized Earth transfer.
mu=398600.435507e9;mus=1.32712440041279419e20;AU=149597870700.;rs=7e6;RS=9e8;beta=540.
alo=2*beta/250000.;ahi=2*beta/50000.
earth=dict(mu_earth_m3_s2=mu,parking_radius_m=rs,chosen_soi_radius_m=RS,useful_power_per_initial_mass_W_kg=beta,
           initial_accel_min=alo,initial_accel_max=ahi,
           initial_epsilon_min=alo*rs*rs/mu,initial_epsilon_max=ahi*rs*rs/mu,
           radius_for_epsilon_0p05_m=math.sqrt(.05*mu/alo),
           epsilon_at_chosen_soi_lower=alo*RS*RS/mu,
           leading_solar_tide_ratio_at_soi=2*mus/mu*(RS/AU)**3)
result={'convention':'Planar, useful fixed ship power, ideal or full-power/coast band as labeled; no interval certification.',
        'case1_averaged_spirals':case1,'case2_exact_escape_capture':case2,'case3_soi_handoff':case3,
        'earth_model_validity_illustration':earth,
        'checks':'scalar integrals vs quadrature; scalar profile vs time ODE; time monotonicity; exact vector ODE; work and angular identities; reversed capture; analytic band enclosure; chart finite differences; covector scaling'}
(HERE/'test_cases.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
