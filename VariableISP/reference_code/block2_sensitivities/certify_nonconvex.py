"""Directed-rounding interval certificate for a CW Jensen coefficient.

No external packages. Trig intervals use exact rational Taylor sums with
an explicit Lagrange remainder; every interval operation rounds outward.
This certifies the leading coefficient used in the small-parameter
nonconvexity proof, not a finite-epsilon nonlinear global optimum.
"""
from decimal import Decimal, localcontext, ROUND_FLOOR, ROUND_CEILING
from fractions import Fraction
from math import factorial
import json
from pathlib import Path

PREC=70
def down(f):
    with localcontext() as c:c.prec=PREC;c.rounding=ROUND_FLOOR;return +f()
def up(f):
    with localcontext() as c:c.prec=PREC;c.rounding=ROUND_CEILING;return +f()
class I:
    def __init__(self,a,b=None):
        self.lo=Decimal(str(a));self.hi=self.lo if b is None else Decimal(str(b))
    @staticmethod
    def cast(a):return a if isinstance(a,I) else I(a)
    def __add__(self,x):
        x=I.cast(x);return I(down(lambda:self.lo+x.lo),up(lambda:self.hi+x.hi))
    __radd__=__add__
    def __neg__(self):return I(self.hi.copy_negate(),self.lo.copy_negate())
    def __sub__(self,x):return self+-I.cast(x)
    def __rsub__(self,x):return I.cast(x)+-self
    def __mul__(self,x):
        x=I.cast(x);pairs=[(a,b) for a in (self.lo,self.hi) for b in (x.lo,x.hi)]
        return I(min(down(lambda:a*b) for a,b in pairs),max(up(lambda:a*b) for a,b in pairs))
    __rmul__=__mul__
    def __truediv__(self,x):
        x=I.cast(x)
        if x.lo<=0<=x.hi:raise ValueError('interval division through zero')
        inv=I(down(lambda:Decimal(1)/x.hi),up(lambda:Decimal(1)/x.lo))
        return self*inv
    def __rtruediv__(self,x):return I.cast(x)/self
    def __pow__(self,k):
        if k<0:return 1/(self**(-k))
        v=I(1)
        for _ in range(k):v=v*self
        return v
    def pair(self):return [str(self.lo),str(self.hi)]

def frac_lower(q):return down(lambda:Decimal(q.numerator)/Decimal(q.denominator))
def frac_upper(q):return up(lambda:Decimal(q.numerator)/Decimal(q.denominator))
def trig(x,cos=False):
    x=Fraction(x);N=90
    # Polynomial through degree 181 (sin) or 180 (cos).
    val=sum(((-1)**k)*x**(2*k+(0 if cos else 1))/factorial(2*k+(0 if cos else 1)) for k in range(N+1))
    deg=2*N+(0 if cos else 1)
    rem=abs(x)**(deg+1)/factorial(deg+1)
    return I(frac_lower(val-rem),frac_upper(val+rem))

def coefficient(hstr):
    h=I(hstr);S=trig(hstr);C=trig(hstr,True);S2=trig(2*Fraction(hstr))
    W=[[I(0) for _ in range(4)] for _ in range(4)]
    W[0][0]=I('6.5')*h-8*S+I('.75')*S2
    W[0][1]=-3*(h-S)**2
    W[0][2]=4*(1-C)-I('1.5')*S**2
    W[0][3]=-11*h+14*S-I('1.5')*S2
    W[1][1]=3*h**3+14*h+24*h*C-32*S-3*S2
    W[1][2]=5*h+6*h*C-8*S-I('1.5')*S2
    W[1][3]=6*S**2+4*(1-C)-12*h*S+I('4.5')*h**2
    W[2][2]=I('2.5')*h-I('.75')*S2
    W[2][3]=3*S**2+6*(C-1)
    W[3][3]=19*h+3*S2-24*S
    for i in range(4):
        for j in range(i):W[i][j]=W[j][i]
    # Phi(h)*(1,1,0,-3/2)=(1,1-3h/2,0,-3/2).
    y=[I(1),1-I('1.5')*h,I(0),I('-1.5')]
    a=[W[i][:]+[y[i]] for i in range(4)]
    for k in range(4):
        assert a[k][k].lo>0
        for i in range(k+1,4):
            f=a[i][k]/a[k][k]
            for j in range(k+1,5):a[i][j]=a[i][j]-f*a[k][j]
    sol=[I(0) for _ in range(4)]
    for i in range(3,-1,-1):sol[i]=(a[i][4]-sum(a[i][j]*sol[j] for j in range(i+1,4)))/a[i][i]
    return sum(y[i]*sol[i] for i in range(4))

if __name__=='__main__':
    vals=[coefficient(h) for h in ['10','10.5','11']]
    gap=vals[1]-(vals[0]+vals[2])/2
    assert gap.lo>0
    result={'h':[10,10.5,11],'coefficient_intervals':[v.pair() for v in vals],
            'jensen_gap_interval':gap.pair(),'strictly_positive':True,'decimal_precision':PREC,
            'meaning':'Rigorous leading coefficient of the small-epsilon exact-Kepler global-value counterexample.'}
    Path(__file__).with_name('nonconvexity_certificate.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
