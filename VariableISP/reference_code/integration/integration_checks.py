"""Cross-package identities. Numerical regression checks, not interval proofs."""
from pathlib import Path
import importlib.util, json, math, subprocess
import numpy as np

HERE=Path(__file__).resolve().parent
ROOT=HERE.parent
def package(name):
    p=ROOT/name
    return p if p.exists() else ROOT/'handoff'/name
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module
esc=load('escape_reference',package('block6_escape_capture')/'escape_reference.py')
cw=load('cw_reference',package('block1_cw')/'cw_reference.py')

# The same stationary branch must have consistent intrinsic and moving derivatives.
fixture=json.loads((package('block2_sensitivities')/'block2_test_cases.json').read_text())
f=fixture['case2_kappa_0p0032'];omega=f['rho']**-1.5
u0=np.array(f['u0']);uf=np.array(f['uf'])
timing_errors=[abs(f['H']-f['L']-u0@u0),
               abs(f['H']-omega*f['L']-uf@uf),
               abs(f['F_duration_fixed_departure']+uf@uf),
               abs(f['F_departure_fixed_duration']-(u0@u0-uf@uf))]
assert max(timing_errors)<1e-10
coeffs=[cw.cost(h,np.array([1.,1.,0.,-1.5])) for h in (10.,10.5,11.)]
jensen=coeffs[1]-(coeffs[0]+coeffs[2])/2
assert abs(jensen-0.0000763028630103681)<1e-14

# Dimensionless phases with different action units must propagate one physical mass.
m0=1000.;powers=[1e6,1e6];efforts=[20.,40.];scales=[2.,5.]
def propagate(ps):
    mass=m0;rows=[]
    for j,P,J0 in zip(efforts,ps,scales):
        chi=2*P/(mass*J0);Jn=j/J0
        next_mass=mass*chi/(chi+Jn)
        rows.append(dict(entry_mass=mass,chi=chi,J=Jn,J0=J0,P=P,exit_mass=next_mass))
        mass=next_mass
    return mass,rows
mf,rows=propagate(powers)
truth=1/(1/m0+sum(j/(2*p) for j,p in zip(efforts,powers)))
assert abs(mf-truth)<2e-13
mf_different,rows_different=propagate([1e6,2e6])
truth_different=1/(1/m0+efforts[0]/2e6+efforts[1]/4e6)
assert abs(mf_different-truth_different)<2e-13
wrong_reset=1/(1/m0+efforts[1]/2e6)
assert wrong_reset-mf>9

# Check chart stationarity against the independently formed Hamiltonian jump.
rng=np.random.default_rng(20261029)
chart_max=saltation_max=relative_H_max=pairing_max=0.
for i in range(256):
    theta=rng.uniform(-math.pi,math.pi);R=rng.uniform(.2,2.)
    er=np.array([math.cos(theta),math.sin(theta)]);et=np.array([-er[1],er[0]])
    x=R*er
    vr=(1 if i%2 else -1)*rng.uniform(.2,2.)
    rel=vr*er+rng.normal()*et
    Vp,Ap,dG,pv,pr=rng.normal(size=(5,2))
    z,prp,dH=esc.soi_jump(x,rel,Vp,dG,pv,pr)
    # Same unique engine selector: optimized control gain cancels.
    dH_ind=(prp-pr)@(Vp+rel)+pv@dG
    chart=esc.interface_gradient(x,rel,Vp,Ap,pr,pv,0.,prp,pv,dH_ind)
    chart_max=max(chart_max,float(np.max(np.abs(chart))))
    relative_H_max=max(relative_H_max,abs(dH_ind-(prp-pr)@Vp))
    n=np.r_[x,np.zeros(2)]
    deltaf=np.r_[np.zeros(2),dG]
    S=np.eye(4)+np.outer(deltaf,n)/(x@rel)
    saltation_max=max(saltation_max,float(np.max(np.abs(S.T@np.r_[prp,pv]-np.r_[pr,pv]))))
    scales_local=esc.scale_factors(10**rng.uniform(-4,-1),10**rng.uniform(-7,-3))
    l=scales_local['length'];cv=scales_local['velocity'];cj=scales_local['effort'];ct=scales_local['time']
    dx,dv=rng.normal(size=(2,2));dt=rng.normal();H=rng.normal()
    local=pr@dx+pv@dv-H*dt
    sun=(cj/l*pr)@(l*dx)+(cj/cv*pv)@(cv*dv)-(cj/ct*H)*(ct*dt)
    pairing_max=max(pairing_max,abs(sun-cj*local)/max(1.,abs(cj*local)))
assert chart_max<2e-13 and saltation_max<2e-13
assert relative_H_max<2e-13 and pairing_max<1e-13
# Explicitly reject a grazing chart.
try:esc.soi_jump(np.array([1.,0.]),np.array([0.,1.]),np.zeros(2),np.ones(2),np.ones(2),np.ones(2))
except ValueError:pass
else:raise AssertionError('grazing accepted')

(HERE/'results').mkdir(exist_ok=True)
binary=HERE/'results'/'test_integration'
command=['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',
         '-I',str(ROOT),'-I',str(ROOT/'handoff'),str(HERE/'test_integration.cpp'),'-o',str(binary)]
subprocess.run(command,check=True)
run=subprocess.run([str(binary)],check=True,text=True,capture_output=True)
binary.unlink()
out=dict(scope='Double-precision integration checks; no general trajectory certification.',
         timing_identity_max_error=max(timing_errors),nonconvex_coefficients=coeffs,jensen_gap=jensen,
         mass_equal_power=dict(final_mass=mf,phases=rows,incorrect_reset_mass=wrong_reset),
         mass_unequal_power=dict(final_mass=mf_different,phases=rows_different),
         transverse_crossings=256,soi_chart_max_error=chart_max,
         saltation_max_error=saltation_max,relative_H_max_error=relative_H_max,
         normalized_action_pairing_relative_error=pairing_max,cpp=run.stdout.strip(),passed=True)
(HERE/'results'/'integration_summary.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
