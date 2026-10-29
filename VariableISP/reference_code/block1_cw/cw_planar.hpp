#pragma once
// Dimensionless planar CW math. C++17, no third-party dependencies.
// Exact model formulas; floating-point calculations are not interval certificates.
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <algorithm>

namespace cw {
using Vec = std::array<double,4>;
using Mat = std::array<std::array<double,4>,4>;
inline double dot(const Vec&a,const Vec&b) {
  double v=0; for(int i=0;i<4;++i)v+=a[i]*b[i]; return v;
}
inline Vec mv(const Mat&a,const Vec&v) {
  Vec y{};for(int i=0;i<4;++i)for(int j=0;j<4;++j)y[i]+=a[i][j]*v[j];return y;
}
inline Mat phi(double h) {
  const double s=std::sin(h),c=std::cos(h),d=2*std::pow(std::sin(h/2),2);
  double smh=s-h;
  if(std::abs(h)<.1) {
    double term=-h*h*h/6;smh=term;
    for(int k=2;k<=8;++k){term*=-h*h/((2*k)*(2*k+1));smh+=term;}
  }
  return {{{1+3*d,0,s,2*d},{6*smh,1,-2*d,h+4*smh},
           {3*s,0,c,2*s},{-6*d,0,-2*s,1-4*d}}};
}
inline Vec scaling(double h) {
  if(!(h>0) || !std::isfinite(h))throw std::invalid_argument("positive finite h required");
  double r=std::sqrt(h);return {h*r,h*r,r,r};
}
inline Mat scaled_gramian(double h) {
  const Vec d=scaling(h);Mat w{};
  if(h<.1) {
    static constexpr double coeff[10][17]={
#include "cw_small_h.inc"
    };
    int idx=0;
    for(int i=0;i<4;++i)for(int j=i;j<4;++j){
      double v=coeff[idx][16];for(int k=15;k>=0;--k)v=v*h+coeff[idx][k];
      w[i][j]=w[j][i]=v;++idx;
    }
    return w;
  }
  double s=std::sin(h),c=std::cos(h),s2=std::sin(2*h);
  w[0][0]=6.5*h-8*s+.75*s2;
  w[0][1]=-3*(h-s)*(h-s);
  w[0][2]=4*(1-c)-1.5*s*s;
  w[0][3]=-11*h+14*s-1.5*s2;
  w[1][1]=3*h*h*h+14*h+24*h*c-32*s-3*s2;
  w[1][2]=5*h+6*h*c-8*s-1.5*s2;
  w[1][3]=6*s*s+4*(1-c)-12*h*s+4.5*h*h;
  w[2][2]=2.5*h-.75*s2;
  w[2][3]=3*s*s+6*(c-1);
  w[3][3]=19*h+3*s2-24*s;
  for(int i=0;i<4;++i)for(int j=i;j<4;++j)w[j][i]=w[i][j]/=d[i]*d[j];
  return w;
}
inline Mat gramian(double h) {
  Mat w=scaled_gramian(h);Vec d=scaling(h);
  for(int i=0;i<4;++i)for(int j=0;j<4;++j)w[i][j]*=d[i]*d[j];
  return w;
}
struct Correction {double cost;double derivative;Vec lambda;};
inline Correction correction_data(double h,const Vec&z) {
  Vec d=scaling(h),y=mv(phi(h),z),v{},lam{};Mat w=scaled_gramian(h),l{};
  for(int i=0;i<4;++i)for(int j=0;j<=i;++j){
    double t=w[i][j];for(int k=0;k<j;++k)t-=l[i][k]*l[j][k];
    if(i==j){if(!(t>0))throw std::runtime_error("Gramian Cholesky failed");l[i][j]=std::sqrt(t);}
    else l[i][j]=t/l[j][j];
  }
  for(int i=0;i<4;++i){double t=y[i]/d[i];for(int j=0;j<i;++j)t-=l[i][j]*v[j];v[i]=t/l[i][i];}
  double c=dot(v,v);
  for(int i=3;i>=0;--i){double t=v[i];for(int j=i+1;j<4;++j)t-=l[j][i]*lam[j];lam[i]=t/l[i][i];}
  for(int i=0;i<4;++i)lam[i]/=d[i];
  return {c,-lam[2]*lam[2]-lam[3]*lam[3],lam};
}
inline std::array<double,2> correction(double t,double h,const Vec&lambda) {
  Mat p=phi(h-t);std::array<double,2>a{};
  for(int i=0;i<4;++i){a[0]+=p[i][2]*lambda[i];a[1]+=p[i][3]*lambda[i];}return a;
}
inline double game_time(const Vec&z,double bp,double be) {
  if(bp<0||be<0||!std::isfinite(bp)||!std::isfinite(be))throw std::invalid_argument("finite nonnegative budgets required");
  if(dot(z,z)==0)return 0;
  if(bp<=be)return std::numeric_limits<double>::infinity();
  const double gap=(bp-be)/(std::sqrt(bp)+std::sqrt(be)),target=gap*gap;
  double hi=1;
  while(correction_data(hi,z).cost>target){hi*=2;if(!std::isfinite(hi))throw std::overflow_error("time bracket");}
  double lo=hi/2;
  while(correction_data(lo,z).cost<=target){hi=lo;lo/=2;}
  for(int i=0;i<60;++i){double mid=(lo+hi)/2;if(mid==lo||mid==hi)break;
    if(correction_data(mid,z).cost>target)lo=mid;else hi=mid;}
  return hi;
}
struct Certificate {double C,D,E,Nmax,L,G1,G2,tube_lhs,Gamma,required_bp;bool tube_valid;};
inline Certificate certificate(double h,const Vec&z,const Vec&se,double be,double eps) {
  if(!(eps>0&&eps<1)||be<0)throw std::invalid_argument("certificate arguments");
  double g=h+h*h+2*h*h*h/3;
  double g1=h*h/2+h*h*h/3+std::pow(h,4)/6;
  double g2=std::pow(h,3)/3+std::pow(h,4)/2+7*std::pow(h,5)/15+2*std::pow(h,6)/9+4*std::pow(h,7)/63;
  auto envelope=[&](const Vec&s){return std::hypot(s[0],s[1])+(1.5*h*h+h*h*h)*std::abs(s[0])+g*std::hypot(s[2],s[3]);};
  double c=correction_data(h,z).cost,d=envelope(z)+std::sqrt(c*g2),e=envelope(se)+std::sqrt(be*g2);
  double nm=3*eps*eps/std::pow(1-eps,4),lip=6*eps/std::pow(1-eps,4);
  double tube=e+d+nm*g1,gamma=lip*std::sqrt(h)*d;
  double need=std::pow(std::sqrt(be)+std::sqrt(c)+gamma,2);
  return {c,d,e,nm,lip,g1,g2,tube,gamma,need,tube<eps};
}
} // namespace cw
