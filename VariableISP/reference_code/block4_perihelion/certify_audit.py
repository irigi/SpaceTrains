"""Exact rational verification for the two Bezier audit fixtures; no float arithmetic."""
from fractions import Fraction as F
from math import comb
import json
from pathlib import Path
def dot(a,b):return sum(x*y for x,y in zip(a,b))
def split(p):
 mid=lambda a,b:tuple((x+y)/2 for x,y in zip(a,b))
 a,b,c=[mid(p[i],p[i+1]) for i in range(3)];d,e=mid(a,b),mid(b,c);f=mid(d,e)
 return [p[0],a,d,f],[f,e,c,p[3]]
def audit(p,R,E,n=12):
 cs=[sum(F(comb(3,i)*comb(3,k-i),comb(6,k))*dot(p[i],p[k-i]) for i in range(4) if 0<=k-i<4) for k in range(7)]
 if min(cs)>=(R+E)**2:return 'safe'
 left,right=split(p)
 if R>E and any(dot(x,x)<(R-E)**2 for x in [p[0],left[-1],p[-1]]):return 'violation'
 if n==0:return 'unresolved'
 results=[audit(q,R,E,n-1) for q in [left,right]]
 return 'violation' if 'violation' in results else ('safe' if results==['safe','safe'] else 'unresolved')
safe=[(-F(1,50),F(101,1000)),(-F(1,150),F(101,1000)),(F(1,150),F(101,1000)),(F(1,50),F(101,1000))]
dip=[(F(x,100),F(0)) for x in [12,8,8,12]]
result={'arithmetic':'exact rational','safe_fixture':audit(safe,F(1,10),F(1,100000)), 'hidden_dip_fixture':audit(dip,F(1,10),F(1,100000))}
assert result['safe_fixture']=='safe' and result['hidden_dip_fixture']=='violation'
Path(__file__).with_name('audit_certificate.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
