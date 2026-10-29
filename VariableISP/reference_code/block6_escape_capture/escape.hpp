#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <limits>
namespace escape_capture {
using V=std::array<double,2>;
inline double dot(V a,V b){return a[0]*b[0]+a[1]*b[1];}
inline double norm(V a){return std::hypot(a[0],a[1]);}
inline V rot(V a){return {-a[1],a[0]};}
inline V sub(V a,V b){return {a[0]-b[0],a[1]-b[1]};}
inline void require(bool x){if(!x)throw std::invalid_argument("invalid escape/capture input");}
inline void inputs(double D,double chi,double lo,double hi){require(std::isfinite(D)&&std::isfinite(chi)&&std::isfinite(lo)&&std::isfinite(hi)&&D>=0&&chi>0&&lo>0&&lo<=hi);}
struct Fixed {double effort,time,mass_fraction;};
inline Fixed fixed_exhaust(double D,double chi,double nu){
 inputs(D,chi,nu,nu);
 double J=chi*std::expm1(D/nu);require(std::isfinite(J));
 return {J,nu*nu/chi*(-std::expm1(-D/nu)),std::exp(-D/nu)};
}
struct Integrals{double progress,time,first_cut,second_cut;};
inline Integrals integrals(double chi,double W,double k,double lo,double hi){
 require(chi>0&&W>=chi&&k>0&&lo>0&&lo<=hi);
 double a=std::clamp(lo/k,chi,W),b=std::clamp(hi/k,a,W);
 return {lo*std::log1p((a-chi)/chi)+k*(b-a)+hi*std::log1p((W-b)/b),
 lo*lo*(a-chi)/(chi*a)+k*k*(b-a)+hi*hi*(W-b)/(b*W),a,b};
}
struct Burn {double time,k,effort;};
inline Burn fastest_at_effort(double D,double chi,double J,double lo,double hi){
 inputs(D,chi,lo,hi);require(J>=0&&std::isfinite(J));
 if(D==0){require(J==0);return {0,0,0};}
 auto slow=fixed_exhaust(D,chi,hi),fast=fixed_exhaust(D,chi,lo);
 double tol=2e-12*std::max(slow.effort,fast.effort),W=chi+J;require(W>chi);
 require(J>=slow.effort-tol&&J<=fast.effort+tol);
 if(std::abs(J-slow.effort)<=tol)return {slow.time,hi/chi,J};
 if(std::abs(J-fast.effort)<=tol)return {fast.time,lo/W,J};
 double kl=lo/W,ku=hi/chi;
 for(int i=0;i<80;++i){double m=(kl+ku)/2;if(integrals(chi,W,m,lo,hi).progress<D)kl=m;else ku=m;}
 double k=(kl+ku)/2;return {integrals(chi,W,k,lo,hi).time,k,J};
}
struct Solution {bool feasible;double effort,burn_time,coast_time,k;};
inline Solution minimum_effort(double D,double h,double chi,double lo,double hi){
 inputs(D,chi,lo,hi);require(h>0&&std::isfinite(h));
 if(D==0)return {true,0,0,h,0};
 auto slow=fixed_exhaust(D,chi,hi),fast=fixed_exhaust(D,chi,lo);
 if(h<fast.time)return {false,0,0,0,0};
 if(h>=slow.time)return {true,slow.effort,slow.time,h-slow.time,hi/chi};
 double jl=slow.effort,ju=fast.effort;
 for(int i=0;i<80;++i){double m=(jl+ju)/2;if(fastest_at_effort(D,chi,m,lo,hi).time>h)jl=m;else ju=m;}
 double J=(jl+ju)/2;auto b=fastest_at_effort(D,chi,J,lo,hi);
 return {true,J,b.time,0,b.k};
}
inline Solution minimum_time(double D,double budget,double chi,double lo,double hi){
 inputs(D,chi,lo,hi);require(budget>=0&&std::isfinite(budget));
 auto slow=fixed_exhaust(D,chi,hi),fast=fixed_exhaust(D,chi,lo);
 if(budget<slow.effort)return {false,0,0,0,0};
 auto b=fastest_at_effort(D,chi,std::min(budget,fast.effort),lo,hi);
 return {true,b.effort,b.time,0,b.k};
}
struct Shape {V x,v,u;};
inline Shape exact_shape(double t){
 require(t>=0&&t<=10);double r=1+.03*t*t,rd=.06*t,th=t/std::sqrt(r),om=1/std::pow(r,1.5);
 V e{std::cos(th),std::sin(th)},et=rot(e);
 return {{r*e[0],r*e[1]},{rd*e[0]+r*om*et[0],rd*e[1]+r*om*et[1]},
         {.06*e[0]+.5*rd*om*et[0],.06*e[1]+.5*rd*om*et[1]}};
}
inline double exact_effort(){return 651./16000+std::acos(-1.)*std::sqrt(3.)/240;}
struct Jump {double multiplier,hamiltonian_jump;V pr_after;};
inline Jump soi_jump(V x,V vrel,V Vp,V dG,V pv,V pr){
 double d=dot(x,vrel);require(d!=0&&std::isfinite(d));double z=-dot(pv,dG)/d;
 return {z,z*dot(x,Vp),{pr[0]+z*x[0],pr[1]+z*x[1]}};
}
inline std::array<double,4> interface_gradient(V x,V vrel,V Vp,V Ap,V prm,V pvm,double Hm,V prp,V pvp,double Hp){
 double R=norm(x);require(R>0);V er{x[0]/R,x[1]/R},et=rot(er),dr=sub(prm,prp),dv=sub(pvm,pvp);
 double vr=dot(vrel,er),vt=dot(vrel,et);V tangent{vr*et[0]-vt*er[0],vr*et[1]-vt*er[1]};
 return {R*dot(dr,et)+dot(dv,tangent),dot(dv,er),dot(dv,et),Hp-Hm+dot(dr,Vp)+dot(dv,Ap)};
}
inline double necessary_effort(double Ei,double Ef,double ji,double jf,double floor,double R,double h,double chi,double nuhi){
 require(floor>0&&R>=floor&&h>0&&chi>0&&nuhi>0&&2*Ei+2/floor>=0&&2*Ef+2/floor>=0);
 double de=std::abs(std::sqrt(2*Ef+2/floor)-std::sqrt(2*Ei+2/floor));
 double d=std::max(de,std::abs(jf-ji)/R);
 return std::max(d*d/h,chi*std::expm1(d/nuhi));
}
}
