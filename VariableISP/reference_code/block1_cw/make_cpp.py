from fractions import Fraction as F
from math import factorial
from pathlib import Path
p=Path(__file__).resolve().parent
A=[[0,0,1,0],[0,0,0,1],[3,0,0,2],[0,0,-2,0]]
coeff=[[[F(0),F(0)],[F(0),F(0)],[F(1),F(0)],[F(0),F(1)]]]
for j in range(1,20):
 coeff.append([[sum(A[r][k]*coeff[-1][k][c] for k in range(4))/j for c in range(2)] for r in range(4)])
arrays=[]
for r in range(4):
 for c in range(r,4):
  shift=2 if r<2 and c<2 else 1 if r<2 or c<2 else 0
  out=[]
  for degree in range(17):
   n=degree+shift
   v=sum(sum(coeff[j][r][k]*coeff[n-j][c][k] for k in range(2)) for j in range(n+1))/F(n+1)
   out.append(format(float(v),'.17g'))
  arrays.append('    {'+', '.join(out)+'}')
(p/'cw_small_h.inc').write_text('// Generated exact Taylor coefficients of D^-1 W D^-1, through h^16.\n'+',\n'.join(arrays)+'\n')
