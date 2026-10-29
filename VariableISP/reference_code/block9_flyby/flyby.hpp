#pragma once
#include <array>
#include <cmath>
#include <stdexcept>
namespace flyby {
using V=std::array<double,2>;using M=std::array<V,2>;
inline double dot(V a,V b){return a[0]*b[0]+a[1]*b[1];}
inline V add(V a,V b){return {a[0]+b[0],a[1]+b[1]};}
inline V sub(V a,V b){return {a[0]-b[0],a[1]-b[1]};}
inline V scale(V a,double k){return {a[0]*k,a[1]*k};}
inline V J(V a){return {-a[1],a[0]};}
inline V rotate(V a,double t){double c=std::cos(t),s=std::sin(t);return {c*a[0]-s*a[1],s*a[0]+c*a[1]};}
inline V transpose(M a,V p){return {a[0][0]*p[0]+a[1][0]*p[1],a[0][1]*p[0]+a[1][1]*p[1]};}
struct Turn {double angle,max_kick;V gradient;};
inline Turn limit(V w,double mu,double rp){
 double s2=dot(w,w);if(s2<=0||mu<=0||rp<=0)throw std::invalid_argument("hyperbolic flyby arguments");
 double k=rp/mu,e=1+k*s2;
 // sqrt((e-1)(e+1)) avoids subtracting nearly equal squares.
 double root=std::sqrt((k*s2)*(2+k*s2));
 return {2*std::asin(1/e),2*std::sqrt(s2)/e,scale(w,-4*k/(e*root))};
}
inline M fixed_radius_jacobian(V w,double mu,double rp,int sign){
 if(sign!=1&&sign!=-1)throw std::invalid_argument("turn side");
 auto d=limit(w,mu,rp);double t=sign*d.angle,c=std::cos(t),s=std::sin(t);
 V j=J(rotate(w,t)),grad=scale(d.gradient,sign);
 return {V{c+j[0]*grad[0],-s+j[0]*grad[1]},V{s+j[1]*grad[0],c+j[1]*grad[1]}};
}
struct Check {bool equal_speed,allowed_turn;double signed_angle,required_radius;};
inline Check check(V wm,V wp,double mu,double rpmin,double speed2_tolerance){
 double sm=dot(wm,wm),sp=dot(wp,wp);
 if(sm<=0||sp<=0||mu<=0||rpmin<=0||speed2_tolerance<0)throw std::invalid_argument("audit arguments");
 bool eq=std::abs(sm-sp)<=speed2_tolerance;
 double t=std::atan2(wm[0]*wp[1]-wm[1]*wp[0],dot(wm,wp));
 double sh=std::sin(std::abs(t)/2);
 double rp=sh==0?INFINITY:mu/sm*(1/sh-1);
 return {eq,eq&&std::abs(t)<=limit(wm,mu,rpmin).angle,t,rp};
}
struct Gradient {V incoming_excess;double angle,epoch;};
inline Gradient objective_gradient(V prm,V prp,V pvm,V pvp,double Hm,double Hp,V planet_v,V planet_a,V wp,double theta){
 return {sub(pvm,rotate(pvp,-theta)),-dot(pvp,J(wp)),
 Hp-Hm+dot(sub(prm,prp),planet_v)+dot(sub(pvm,pvp),planet_a)};
}
}
