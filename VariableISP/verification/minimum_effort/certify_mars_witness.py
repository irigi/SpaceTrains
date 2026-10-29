"""Whole-interval certificate for an explicit 70-day Earth--Mars witness.

No trajectory integration, optimizer or third-party package is used here.
All decimal input strings are exact rationals. Decimal interval operations
round outwards at 60 significant digits, including square roots. The path
is piecewise cubic Hermite; endpoint states are analytic circular states.
The continuous control is defined by u = r'' + r / |r|**3, with mass
reconstructed from the integral of |u|**2. Finite control jumps at knots
are permitted. This is a feasibility/upper-bound proof, not optimality.
"""
from decimal import Decimal, localcontext, ROUND_FLOOR, ROUND_CEILING
from pathlib import Path
import json
import math

PREC = 60
def down(f):
    with localcontext() as c:
        c.prec = PREC; c.rounding = ROUND_FLOOR
        return +f()
def up(f):
    with localcontext() as c:
        c.prec = PREC; c.rounding = ROUND_CEILING
        return +f()
class I:
    def __init__(self, lo, hi=None):
        self.lo=Decimal(str(lo)); self.hi=self.lo if hi is None else Decimal(str(hi))
        assert self.lo <= self.hi
    @staticmethod
    def cast(x): return x if isinstance(x,I) else I(x)
    def __add__(self,x):
        x=I.cast(x); return I(down(lambda:self.lo+x.lo),up(lambda:self.hi+x.hi))
    __radd__=__add__
    def __neg__(self): return I(self.hi.copy_negate(),self.lo.copy_negate())
    def __sub__(self,x): return self+-I.cast(x)
    def __rsub__(self,x): return I.cast(x)+-self
    def __mul__(self,x):
        x=I.cast(x); pairs=[(a,b) for a in [self.lo,self.hi] for b in [x.lo,x.hi]]
        return I(min(down(lambda:a*b) for a,b in pairs),max(up(lambda:a*b) for a,b in pairs))
    __rmul__=__mul__
    def __truediv__(self,x):
        x=I.cast(x); assert x.lo>0 or x.hi<0
        return self*I(down(lambda:Decimal(1)/x.hi),up(lambda:Decimal(1)/x.lo))
    def __rtruediv__(self,x): return I.cast(x)/self
    def square(self):
        a,b=self.lo,self.hi
        lo=Decimal(0) if a<=0<=b else min(down(lambda:a*a),down(lambda:b*b))
        return I(lo,max(up(lambda:a*a),up(lambda:b*b)))
    def sqrt(self):
        assert self.lo>=0
        # Decimal.sqrt is correctly rounded to nearest; one ulp outward
        # therefore encloses the exact root regardless of its direction.
        with localcontext() as c:
            c.prec=PREC
            lo=self.lo.sqrt().next_minus() if self.lo else Decimal(0)
            hi=self.hi.sqrt().next_plus()
        return I(lo,hi)
    def pair(self): return [str(self.lo),str(self.hi)]

def certify(subdivisions=128):
    base=Path(__file__).resolve().parent
    data=json.loads((base/'mars_witness_knots.json').read_text())
    AU=I('149597870700'); mu=I('1.32712440018e20')
    T0=(AU*AU*AU/mu).sqrt(); V0=(mu/AU).sqrt(); J0=V0.square()/T0
    P=I('1e9'); m0=I('3e6'); md=I('1e6'); chi=2*P/m0/J0
    H=I(data['duration_days'])*86400/T0
    R=I(data['target_radius_AU']); q=I(data['target_half_angle_tangent'])
    cs=(1-q.square())/(1+q.square()); sn=2*q/(1+q.square())
    times=[I(k['fraction'])*H for k in data['knots']]
    positions=[[I(x) for x in k['position']] for k in data['knots']]
    velocities=[[I(x) for x in k['velocity']] for k in data['knots']]
    times[0]=I(0);times[-1]=H
    positions[0]=[I(1),I(0)]; velocities[0]=[I(0),I(1)]
    positions[-1]=[R*cs,R*sn];velocities[-1]=[-sn/R.sqrt(),cs/R.sqrt()]
    effort=I(0); rlo=Decimal('Infinity'); rhi=Decimal('-Infinity')
    velo=Decimal('Infinity'); vehi=Decimal('-Infinity')
    alo=Decimal('Infinity'); ahi=Decimal('-Infinity')
    for j in range(len(times)-1):
        dt=times[j+1]-times[j];assert dt.lo>0
        coeff=[]
        for k in range(2):
            x0,x1=positions[j][k],positions[j+1][k]
            v0,v1=velocities[j][k],velocities[j+1][k]
            coeff.append([x0,dt*v0,3*(x1-x0)-dt*(2*v0+v1),2*(x0-x1)+dt*(v0+v1)])
        for k in range(subdivisions):
            left=I(k)/subdivisions;right=I(k+1)/subdivisions
            s=I(left.lo,right.hi)
            pos=[a+s*(b+s*(c+s*d)) for a,b,c,d in coeff]
            acc=[(2*c+6*d*s)/dt.square() for a,b,c,d in coeff]
            r2=sum(x.square() for x in pos);radius=r2.sqrt()
            control=[a+x/(r2*radius) for a,x in zip(acc,pos)]
            a2=sum(x.square() for x in control); amag=a2.sqrt()
            increment=a2*dt/subdivisions
            # Effort is monotone within every subinterval.
            w=chi+I(effort.lo,up(lambda:effort.hi+increment.hi))
            exhaust=V0*w/amag
            assert radius.lo>I('.1').hi
            assert exhaust.lo>50000 and exhaust.hi<250000
            effort+=increment
            rlo=min(rlo,radius.lo);rhi=max(rhi,radius.hi)
            velo=min(velo,exhaust.lo);vehi=max(vehi,exhaust.hi)
            alo=min(alo,amag.lo);ahi=max(ahi,amag.hi)
    budget=2*P*(1/md-1/m0)/J0
    assert effort.hi<budget.lo
    assert effort.hi<Decimal('2.6')
    assert effort.lo>Decimal('2.5212') and effort.hi<Decimal('2.5307')
    assert rlo>Decimal('.999')
    assert velo>Decimal('99000') and vehi<Decimal('102000')
    fuel=m0-m0*chi/(chi+effort)
    assert fuel.lo>Decimal('1201400') and fuel.hi<Decimal('1204100')
    phase=2*math.atan(float(data['target_half_angle_tangent']))
    initial_phase=phase-float(H.lo)/(float(R.lo)**1.5)
    out={'passed':True,'duration_days':data['duration_days'],
         'target_radius_AU':data['target_radius_AU'],
         'departure_phase_ahead_deg_approx':math.degrees(initial_phase),
         'arrival_polar_angle_deg_approx':math.degrees(phase),
         'hermite_segments':len(times)-1,'subintervals_per_segment':subdivisions,
         'effort_enclosure':effort.pair(),'effort_upper_bound':'2.6',
         'available_effort_budget':budget.pair(),'propellant_kg_enclosure':fuel.pair(),
         'radius_AU_enclosure':I(rlo,rhi).pair(),
         'exhaust_m_per_s_enclosure':I(velo,vehi).pair(),
         'normalized_acceleration_enclosure':I(alo,ahi).pair(),
         'claim':'Exact analytic endpoint states; continuous-time finite-band feasibility; T_star <= 70 days for the specified circular target phase. No global optimality or band-delay estimate.'}
    (base/'mars_witness_certificate.json').write_text(json.dumps(out,indent=2))
    print(json.dumps(out,indent=2))
    return out
if __name__=='__main__':certify()
