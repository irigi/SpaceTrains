#pragma once
#include <array>
#include <cmath>
#include <stdexcept>
namespace powered_flyby {
using V=std::array<double,2>;
inline double dot(V a,V b){return a[0]*b[0]+a[1]*b[1];}
inline V add(V a,V b){return {a[0]+b[0],a[1]+b[1]};}
inline V scale(V a,double k){return {k*a[0],k*a[1]};}
inline V J(V a){return {-a[1],a[0]};}
inline double cross(V a,V b){return a[0]*b[1]-a[1]*b[0];}
inline double energy(V x,V v,double mu=1){return dot(v,v)/2-mu/std::sqrt(dot(x,x));}
// Extrapolated planetary Kepler asymptote. incoming=true selects the past tail.
inline V asymptote(V x,V v,bool incoming,double mu=1){
 double r=std::sqrt(dot(x,x)),E=energy(x,v,mu);
 if(r<=0||mu<=0||E<=0)throw std::domain_error("positive-energy planetary endpoint required");
 double s=std::sqrt(2*E),h=cross(x,v);
 V ev=scale(add(scale(x,dot(v,v)-mu/r),scale(v,-dot(x,v))),1/mu);
 double e=std::sqrt(dot(ev,ev));V n=scale(ev,1/e);
 return scale(add(scale(n,incoming?1:-1),scale(J(n),h*s/mu)),s/e);
}
inline double energy_effort_lower(double sin,double sout,double qmin,double duration,double mu=1){
 if(sin<0||sout<0||qmin<=0||duration<=0||mu<=0)throw std::invalid_argument("bound arguments");
 double d=std::sqrt(sout*sout+2*mu/qmin)-std::sqrt(sin*sin+2*mu/qmin);
 return d*d/duration;
}
inline double angular_effort_lower(double hminus,double hplus,double soi,double duration){
 if(soi<=0||duration<=0)throw std::invalid_argument("bound arguments");
 double d=hplus-hminus;return d*d/(soi*soi*duration);
}
struct Crossing {double multiplier;V p_position_after;double hamiltonian_jump;};
// Same common dimensional/normalized coordinates for both modes. Unique selector,
// unchanged mass/power, and a transverse crossing are assumed.
inline Crossing soi_crossing(V x,V v_relative,V planet_velocity,V delta_gravity,V p_velocity,V p_position_before){
 double den=dot(x,v_relative);if(den==0)throw std::domain_error("grazing SOI crossing");
 double z=-dot(p_velocity,delta_gravity)/den;
 return {z,add(p_position_before,scale(x,z)),z*dot(x,planet_velocity)};
}
}
