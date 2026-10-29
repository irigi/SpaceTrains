"""Exact rational enclosures for the worked solar case's rounded inequalities."""
from fractions import Fraction as F
from math import isqrt,factorial
import json
from pathlib import Path
class I:
 def __init__(self,a,b=None):self.a=F(a);self.b=self.a if b is None else F(b)
 @staticmethod
 def cast(x):return x if isinstance(x,I) else I(x)
 def __add__(self,x):x=I.cast(x);return I(self.a+x.a,self.b+x.b)
 __radd__=__add__
 def __neg__(self):return I(-self.b,-self.a)
 def __sub__(self,x):return self+-I.cast(x)
 def __rsub__(self,x):return I.cast(x)+-self
 def __mul__(self,x):
  x=I.cast(x);v=[a*b for a in [self.a,self.b] for b in [x.a,x.b]];return I(min(v),max(v))
 __rmul__=__mul__
 def __truediv__(self,x):x=I.cast(x);assert x.a>0 or x.b<0;return self*I(1/x.b,1/x.a)
 def __rtruediv__(self,x):return I.cast(x)/self
 def __pow__(self,n):
  z=I(1)
  for _ in range(n):z=z*self
  return z
 def sqrt(self):
  assert self.a>=0;scale=10**40
  lo=isqrt(self.a.numerator*scale**2//self.a.denominator);hi=isqrt(self.b.numerator*scale**2//self.b.denominator)+1
  return I(F(lo,scale),F(hi,scale))
 def pair(self):return [float(self.a),float(self.b)]
def trig(x,cos=False):
 z=I(0)
 for k in range(11):
  n=2*k+(0 if cos else 1);z+=(-1)**k*x**n/factorial(n)
 d=20 if cos else 21;rem=max(abs(x.a),abs(x.b))**(d+1)/factorial(d+1)
 return z+I(-rem,rem)
c=F(2,5);H=F(1,20);U=4*c*c*H+c*c*H**3/3;r=1+c*H*H
theta=H/I(r).sqrt();delta=theta-H
dr2=r*r+1-2*r*trig(delta,True)
dv2=(2*c*H)**2+F(1,r)+1-2*(2*c*H*trig(delta)+trig(delta,True)/I(r).sqrt())
assert dr2.b<F('0.00101')**2
assert dv2.a>F('.0399')**2 and dv2.b<F('.041')**2
K=2/F('.9')**3;x=K*H*H
cosh=sum(x**k/F(factorial(2*k)) for k in range(11))
rem=x**11/F(factorial(22))/(1-x/F(23*24));assert (cosh+rem)**2<F('1.007')
assert U*H**3*F('1.007')/3<F('.0012')**2
xb=K*F('.001')**2
cb=sum(xb**k/F(factorial(2*k)) for k in range(6))+xb**6/F(factorial(12))/(1-xb/F(13*14))
assert (F('.00101')+F('.041')*F('.001'))*cb<F('.0011')
assert (F('.00101')+F('.041')*H)*F('1.004')<F('.0031')
assert K*F('.0011')*F('.001')<F('.0000031')
lower=(F('.0399')-F('.0000031'))**2/(F('.049')*F('1.007'));assert lower>U
assert F('.0399')**2/(H*F('1.007'))>F('.0316')
# Exhaust bounds with exact physical parameters and coarse speed enclosures.
P=F(10**9);m0=F(3*10**6);AU=F(149597870700);mu=F('1.32712440018e20')
chiV=2*P*AU*AU/(m0*mu)
assert chiV/F('.801')>140000
assert (chiV+30000*U)/F('.8')<142000
out={'all_exact_inequalities_pass':True,'delta_r_squared':dr2.pair(),'delta_v_squared':dv2.pair(),'lower_effort_at_0.049':float(lower),'budget':float(U),'certified_exhaust_enclosure_kms':[140,142]}
Path(__file__).with_name('worked_certificate.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
