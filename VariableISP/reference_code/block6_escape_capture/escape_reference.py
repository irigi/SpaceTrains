"""Block 6: phase-free monotone averaged spirals and exact feasible shapes.
All floating point outputs are numerical checks, not interval certificates.
"""
import math
import numpy as np
from scipy.integrate import quad
from scipy.optimize import brentq


def circular_progress(ri,rf,mu=1.):
    if min(ri,rf,mu)<=0: raise ValueError('positive radii and mu required')
    return abs(math.sqrt(mu/ri)-math.sqrt(mu/rf))


def fixed_exhaust(D,chi,nu):
    if D<0 or min(chi,nu)<=0: raise ValueError('invalid spiral inputs')
    return dict(effort=chi*math.expm1(D/nu),
                burn_time=nu*nu/chi*(-math.expm1(-D/nu)),
                mass_fraction=math.exp(-D/nu))


def progress_time(chi,W,k,nlo,nhi):
    """Integrate z(w)=clip(k,nlo/w,nhi/w): D=int z dw; T=int z^2 dw."""
    if not (0<chi<=W and 0<nlo<=nhi and k>0):raise ValueError('invalid profile')
    a=max(chi,min(W,nlo/k)); b=max(a,min(W,nhi/k))
    D=nlo*math.log1p((a-chi)/chi)+k*(b-a)+nhi*math.log1p((W-b)/b)
    T=nlo*nlo*(a-chi)/(chi*a)+k*k*(b-a)+nhi*nhi*(W-b)/(b*W)
    return D,T,a,b


def fastest_at_effort(D,chi,J,nlo,nhi):
    """Fastest monotone averaged burn at fixed effort J; no inserted coasts."""
    if min(chi,nlo,nhi)<=0 or nlo>nhi or D<0 or J<0:raise ValueError('invalid inputs')
    if D==0:
        if J==0:return dict(time=0.,k=None,W=chi,segments=[])
        raise ValueError('positive effort with zero monotone progress')
    W=chi+J
    if W==chi:raise ValueError('mass increment below floating-point resolution')
    low=fixed_exhaust(D,chi,nhi); high=fixed_exhaust(D,chi,nlo)
    tol=2e-12*max(low['effort'],high['effort'])
    if J<low['effort']-tol or J>high['effort']+tol:raise ValueError('effort outside monotone feasible range')
    if abs(J-low['effort'])<=tol:
        return dict(time=low['burn_time'],k=nhi/chi,W=W,segments=[['max_exhaust',chi,W]])
    if abs(J-high['effort'])<=tol:
        return dict(time=high['burn_time'],k=nlo/W,W=W,segments=[['min_exhaust',chi,W]])
    k=brentq(lambda k:progress_time(chi,W,k,nlo,nhi)[0]-D,nlo/W,nhi/chi,xtol=1e-11,rtol=1e-14)
    _,T,a,b=progress_time(chi,W,k,nlo,nhi)
    parts=[]
    for label,l,r in [('min_exhaust',chi,a),('constant_acceleration',a,b),('max_exhaust',b,W)]:
        if r-l>1e-13*chi:parts.append([label,l,r])
    return dict(time=T,k=k,W=W,segments=parts)


def minimum_effort(D,h,chi,nlo,nhi):
    if h<=0:raise ValueError('positive duration required')
    if D==0:return dict(effort=0.,burn_time=0.,coast_time=h,status='coast',segments=[])
    if min(D,chi,nlo,nhi)<=0 or nlo>nhi:raise ValueError('invalid input')
    slow=fixed_exhaust(D,chi,nhi);fast=fixed_exhaust(D,chi,nlo)
    if h<fast['burn_time']-1e-12*fast['burn_time']:
        return dict(status='infeasible_in_monotone_averaged_model')
    if h>=slow['burn_time']:
        return dict(effort=slow['effort'],burn_time=slow['burn_time'],coast_time=h-slow['burn_time'],
                    status='max_exhaust_plus_coast',segments=[['max_exhaust',chi,chi+slow['effort']]])
    J=brentq(lambda J:fastest_at_effort(D,chi,J,nlo,nhi)['time']-h,
             slow['effort'],fast['effort'],xtol=2e-16,rtol=1e-13)
    p=fastest_at_effort(D,chi,J,nlo,nhi)
    return dict(effort=J,burn_time=p['time'],coast_time=0.,k=p['k'],segments=p['segments'],status='burn_only')


def minimum_time(D,budget,chi,nlo,nhi):
    if budget<0:raise ValueError('negative budget')
    low=fixed_exhaust(D,chi,nhi);high=fixed_exhaust(D,chi,nlo)
    if budget<low['effort']:return dict(status='infeasible_in_monotone_averaged_model')
    J=min(budget,high['effort']);out=fastest_at_effort(D,chi,J,nlo,nhi)
    out.update(effort=J,status='feasible_in_monotone_averaged_model');return out


def phase_constant_accel(q0,qf,h,mu=1.):
    return h*(q0**3+q0*q0*qf+q0*qf*qf+qf**3)/(4*mu)


def exact_shape(t,T=10.,R=4.):
    """Dimensionless mu=1, start r=1: r=1+(R-1)(t/T)^2, theta'=r^-3/2."""
    c=(R-1)/T**2;r=1+c*t*t;rd=2*c*t;rdd=2*c
    th=t/math.sqrt(r);om=r**-1.5
    er=np.array([math.cos(th),math.sin(th)]);et=np.array([-er[1],er[0]])
    x=r*er;v=rd*er+r*om*et
    u=rdd*er+.5*rd*om*et
    return x,v,u


def gravity(r,mu=1.):return -mu*np.asarray(r)/np.linalg.norm(r)**3

def soi_jump(x,vrel,Vp,deltaG,pv,pr):
    d=float(np.dot(x,vrel))
    if d==0:raise ValueError('grazing SOI')
    z=-float(np.dot(pv,deltaG))/d
    return z,np.asarray(pr)+z*np.asarray(x),z*float(np.dot(x,Vp))


def interface_gradient(x,vrel,Vp,Ap,pr_minus,pv_minus,Hminus,pr_plus,pv_plus,Hplus):
    """Chart (theta, radial speed, tangential speed, epoch) at fixed SOI radius."""
    R=np.linalg.norm(x);er=np.asarray(x)/R;et=np.array([-er[1],er[0]])
    vr=vrel@er;vt=vrel@et;dr=pr_minus-pr_plus;dv=pv_minus-pv_plus
    return np.array([dr@(R*et)+dv@(vr*et-vt*er),dv@er,dv@et,
                     Hplus-Hminus+dr@Vp+dv@Ap])


def scale_factors(length_ratio,mu_ratio):
    l=length_ratio;g=mu_ratio
    if min(l,g)<=0:raise ValueError('positive scale ratios required')
    return dict(length=l,time=l**1.5/math.sqrt(g),velocity=math.sqrt(g/l),
                effort=g**1.5/l**2.5)
