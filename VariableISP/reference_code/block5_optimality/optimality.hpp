#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>

// All results are double-precision diagnostics, not interval certificates.
namespace block5 {
using V2=std::array<double,2>;
using State=std::array<double,4>;
inline double norm(V2 v){return std::hypot(v[0],v[1]);}
inline double dot(V2 a,V2 b){return a[0]*b[0]+a[1]*b[1];}
inline double cross(V2 a,V2 b){return a[0]*b[1]-a[1]*b[0];}
inline double positive(double x){return std::max(0.,x);}
inline void require(bool b){if(!b)throw std::invalid_argument("Invalid Block 5 input");}
struct Gains { double kr2,kv2,ar,av; };
inline Gains gains(double h,double L){
  require(std::isfinite(h)&&std::isfinite(L)&&h>0&&L>=0);
  double x=L*h*h;
  // Larger values need scaled hyperbolic functions or interval arithmetic.
  require(x<=100.);
  double cr=0,cv=0,ar=0,av=0,term=1.;
  for(int j=0;j<40;++j){
    double a=2.*j+3,b=a+1;
    cr+=term/a; ar+=term/(a*b);
    cv+=term*x/a; av+=term*x/(a*b);
    term*=4*x/((2.*j+3)*(2.*j+4));
  }
  return {h*h*h*cr,h*(1+cv),h*h*h*h*ar,h*h*(.5+av)};
}
struct Tube {double containment_margin,gamma,eta,gap,lower;bool diagnostic_pass;};
inline Tube tube(double h,double U,double b,double delta,double pmax,
                 std::array<double,3> residual={0,0,0}){
  require(std::isfinite(U)&&std::isfinite(b)&&std::isfinite(delta)&&
          std::isfinite(pmax)&&U>=0&&pmax>=0&&delta>0&&delta<b);
  for(double e:residual)require(std::isfinite(e)&&e>=0);
  double ell=b-delta; auto g=gains(h,2/(ell*ell*ell));
  double margin=delta-2*std::sqrt(U*g.kr2);
  double gamma=1-12*pmax/std::pow(ell,4)*g.ar;
  double eta=residual[0]+std::sqrt(g.ar)*residual[1]+std::sqrt(g.av)*residual[2];
  bool pass=margin>0&&gamma>0;
  double gap=pass?eta*eta/(4*gamma):std::numeric_limits<double>::infinity();
  return {margin,gamma,eta,gap,pass?positive(U-gap):0.,pass};
}
struct Lower {double velocity,position,energy,angular,combined;};
inline Lower lower(State x0,State xf,double h,double a,double U){
  require(std::isfinite(h)&&std::isfinite(a)&&std::isfinite(U)&&h>0&&a>0&&U>=0);
  for(double z:x0)require(std::isfinite(z));
  for(double z:xf)require(std::isfinite(z));
  V2 r0{x0[0],x0[1]},v0{x0[2],x0[3]},rf{xf[0],xf[1]},vf{xf[2],xf[3]};
  require(norm(r0)>=a&&norm(rf)>=a);
  V2 dv{vf[0]-v0[0],vf[1]-v0[1]};
  V2 dr{rf[0]-r0[0]-h*v0[0],rf[1]-r0[1]-h*v0[1]};
  double gm=1/(a*a),lv=std::pow(positive(norm(dv)-gm*h),2)/h;
  double lr=3*std::pow(positive(norm(dr)-gm*h*h/2),2)/(h*h*h);
  double de=(dot(vf,vf)-dot(v0,v0))/2-1/norm(rf)+1/norm(r0);
  double c=dot(v0,v0)+2/a-2/norm(r0);
  double iv=std::pow(h*std::sqrt(U)+std::sqrt(h*h*U+h*c),2);
  double le=iv>0?de*de/iv:0.;
  double R=norm(r0)+h*norm(v0)+gm*h*h/2+std::sqrt(U*h*h*h/3);
  double dl=cross(rf,vf)-cross(r0,v0),lm=dl*dl/(h*R*R);
  return {lv,lr,le,lm,std::max({lv,lr,std::min(U,std::max(le,lm))})};
}
// Row-major Hamiltonian variational matrix for [r,v,pr,pv].
inline std::array<double,64> jacobi_matrix(V2 r,V2 pv){
  double R=norm(r);require(R>0&&std::isfinite(R));
  double rp=dot(r,pv);std::array<double,64> F{};
  for(int i=0;i<2;++i){
    F[i*8+i+2]=1.; F[(i+2)*8+i+6]=.5;F[(i+6)*8+i+4]=-1.;
    for(int j=0;j<2;++j){
      double G=-(i==j)/std::pow(R,3)+3*r[i]*r[j]/std::pow(R,5);
      double K=3*(pv[i]*r[j]+r[i]*pv[j]+(i==j)*rp)/std::pow(R,5)
              -15*rp*r[i]*r[j]/std::pow(R,7);
      F[(i+2)*8+j]=G;F[(i+4)*8+j]=-K;F[(i+4)*8+j+6]=-G;
    }
  }
  return F;
}
}
