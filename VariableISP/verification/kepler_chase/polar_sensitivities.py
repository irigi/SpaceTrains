import numpy as np
from scipy.integrate import solve_ivp
def flow(y,pt):
 r,th,vr,vt,pr,pvr,pvt,I=y
 return np.array([vr,vt/r,vt*vt/r-1/r**2+pvr/2,-vr*vt/r+pvt/2,pt*vt/r**2+pvr*vt**2/r**2-2*pvr/r**3-pvt*vr*vt/r**2,-pr+pvt*vt/r,-pt/r-2*pvr*vt/r+pvt*vr/r,(pvr*pvr+pvt*pvt)/4])
def jac(y,pt):
 r,th,vr,vt,pr,pvr,pvt,I=y;J=np.zeros((8,8));dp=np.zeros(8)
 J[0,2]=1;J[1,0]=-vt/r**2;J[1,3]=1/r
 J[2,0]=-vt*vt/r**2+2/r**3;J[2,3]=2*vt/r;J[2,5]=.5
 J[3,0]=vr*vt/r**2;J[3,2]=-vt/r;J[3,3]=-vr/r;J[3,6]=.5
 J[4,0]=-2*pt*vt/r**3-2*pvr*vt*vt/r**3+6*pvr/r**4+2*pvt*vr*vt/r**3
 J[4,2]=-pvt*vt/r**2;J[4,3]=pt/r**2+2*pvr*vt/r**2-pvt*vr/r**2;J[4,5]=vt*vt/r**2-2/r**3;J[4,6]=-vr*vt/r**2
 J[5,0]=-pvt*vt/r**2;J[5,3]=pvt/r;J[5,4]=-1;J[5,6]=vt/r
 J[6,0]=pt/r**2+2*pvr*vt/r**2-pvt*vr/r**2;J[6,2]=pvt/r;J[6,3]=-2*pvr/r;J[6,5]=-2*vt/r;J[6,6]=vr/r
 J[7,5]=pvr/2;J[7,6]=pvt/2;dp[4]=vt/r**2;dp[6]=-1/r
 return J,dp
def integrate(q):
 S=np.zeros((8,4));S[4:7,:3]=np.eye(3)
 y0=np.r_[1,0,0,1,q[:3],0,S.ravel()]
 def rhs(t,z):
  J,dp=jac(z[:8],q[3]);dS=J@z[8:].reshape(8,4);dS[:,3]+=dp
  return np.r_[flow(z[:8],q[3]),dS.ravel()]
 h=np.exp(q[4]);s=solve_ivp(rhs,[0,h],y0,rtol=2e-12,atol=2e-13,method='DOP853',dense_output=True)
 y=s.y[:8,-1];D=np.column_stack([s.y[8:,-1].reshape(8,4),flow(y,q[3])*h])
 return s,y,D
if __name__=='__main__':
 y=np.array([.8,.1,.2,1.1,2.,3.,4.,0]);p=.7;J,dp=jac(y,p)
 eps=1e-25
 C=np.column_stack([np.imag(flow(y.astype(complex)+1j*eps*np.eye(8)[i],p))/eps for i in range(8)])
 assert np.max(abs(C-J))<1e-12
 assert np.max(abs(np.imag(flow(y,p+1j*eps))/eps-dp))<1e-12
 print('PASS analytic polar Jacobian against complex step')
