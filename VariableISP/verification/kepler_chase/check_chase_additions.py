"""Independent numerical checks of the added CW and ballistic formulas.
These support the analytical proofs; they do not establish general game values.
"""
from pathlib import Path
import json
import numpy as np
from scipy.linalg import expm
from scipy.integrate import quad_vec,solve_ivp
B=np.r_[np.zeros((2,2)),np.eye(2)]
def A(n):return np.array([[0,0,1,0],[0,0,0,1],[3*n*n,0,0,2*n],[0,0,-2*n,0]],float)
def correction(n,h,z):
 a=A(n)
 def K(t):return expm(-a*t)@B
 M=quad_vec(lambda t:K(t)@K(t).T,0,h,epsabs=1e-13,epsrel=1e-12)[0]
 v=np.linalg.solve(M,z)
 return float(z@v),lambda t:K(t).T@v
z=np.array([1e-4,-2e-4,3e-5,-1e-5]);h=.5
C,ah=correction(1,h,z)
moment=quad_vec(lambda t:expm(-A(1)*t)@B@ah(t),0,h,epsabs=1e-13)[0]
assert np.linalg.norm(moment-z)<1e-14
Cfree=12*np.dot(z[:2],z[:2])/h**3+12*np.dot(z[:2],z[2:])/h**2+4*np.dot(z[2:],z[2:])/h
errs=[abs(correction(n,h,z)[0]/Cfree-1) for n in [1e-2,1e-3,1e-4]]
assert errs[2]<errs[1]<errs[0] and errs[2]<1e-4
be=1e-6;eps=.01;J=np.array([[0,-1],[1,0.]])
Nmax=3*eps**2/(1-eps)**4;Lip=6*eps/(1-eps)**4
g=h+h*h+2*h**3/3;G1=h*h/2+h**3/3+h**4/6;G2=h**3/3+h**4/2+7*h**5/15+2*h**6/9+4*h**7/63
F=lambda s:np.linalg.norm(s[:2])+(1.5*h*h+h**3)*abs(s[0])+g*np.linalg.norm(s[2:])
D=F(z)+np.sqrt(C*G2);E=np.sqrt(be*G2)
assert E+D+Nmax*G1<eps
bp=(np.sqrt(be)+np.sqrt(C)+Lip*np.sqrt(h)*D)**2
N=lambda q:-(np.array([1.,0])+q)/np.linalg.norm(np.array([1.,0])+q)**3+np.array([1.,0])-np.array([2.,-1])*q
ue=lambda t:np.sqrt(be/h)*np.array([np.cos(2*t),np.sin(2*t)])
def rhs(t,y):
 e,p=y[:4],y[4:8];uE=ue(t);uP=uE+ah(t)+N(e[:2])-N(p[:2])
 return np.r_[A(1)@e+B@(uE+N(e[:2])),A(1)@p+B@(uP+N(p[:2])),np.dot(uE,uE),np.dot(uP,uP)]
sol=solve_ivp(rhs,[0,h],np.r_[np.zeros(4),-z,0.,0.],method='DOP853',rtol=2e-11,atol=1e-14,dense_output=True)
y=sol.y[:,-1];err=np.linalg.norm(y[:4]-y[4:8]);assert err<1e-11 and y[9]<bp
assert abs(y[8]-be)<1e-14
c200=3*(1000-10*200)**2/200**3;M=(np.sqrt(.04)+np.sqrt(c200))**2
fuel=1000-1/(1/1000+c200/(2e6))
assert abs(c200-.375)<1e-15 and abs(M-.6599489742783178)<1e-14
out={'passed':True,'CW_correction_effort':C,'moment_error':float(np.linalg.norm(moment-z)),'relative_free_limit_errors':errs,'tube_bound':E+D+Nmax*G1,'tube_radius':eps,'pursuer_budget_bound':bp,'actual_pursuer_expenditure':float(y[9]),'nonlinear_rendezvous_error':float(err),'ballistic_c_200':c200,'active_endpoint_M_200':M,'ballistic_endpoint_propellant_kg':fuel}
Path(__file__).with_name('chase_checks.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
