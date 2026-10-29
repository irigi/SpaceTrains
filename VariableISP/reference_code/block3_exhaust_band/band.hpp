#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace band {
struct Selection { double acceleration,gain,costate_rate; int mode; }; // 0 coast,1 max exhaust,2 interior,3 max thrust
inline Selection select(double s,double C,double w,double vmin,double vmax){
 if(s<0||w<=0||vmin<=0||vmax<vmin)throw std::invalid_argument("band arguments");
 double lo=w/vmax,hi=w/vmin;
 double a=C>0?std::clamp(s/(2*C),lo,hi):hi;
 double gain=s*a-C*a*a;
 if(gain<=0)return {0,0,0,0}; // choose coast at a tie
 int mode=a==lo?1:(a==hi?3:2);
 double cdot=mode==2?0:(s-2*C*a)*a/w;
 return {a,gain,cdot,mode};
}
inline bool admissible(double a,double w,double vmin,double vmax){
 return a==0||(a>=w/vmax&&a<=w/vmin);
}
// Conservative whole-interval check from certified enclosures; classify exact coast separately.
inline bool interval_admissible(double amin,double amax,double wmin,double wmax,double vmin,double vmax){
 return amin>=wmax/vmax&&amax<=wmin/vmin;
}
// Exact 1D fixed-exhaust arc costate map: |lambda_v|=L0+nu*t without a sign crossing.
inline double boundary_lambda_m(double lm0,double L0,double nu,double m0,double P,double V,double t){
 double sigma=2*P/(V*V),m=m0-sigma*t;
 if(m<=0||L0<0||L0+nu*t<0)throw std::domain_error("invalid boundary arc");
 double B=nu/sigma,A=L0+B*m0;
 return lm0+V*(A*(1/m-1/m0)+B*std::log(m/m0));
}
}
