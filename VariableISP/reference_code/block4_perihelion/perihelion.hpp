#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace perihelion {
using V=std::array<double,2>; using Curve=std::array<V,4>;
inline double dot(V a,V b){return a[0]*b[0]+a[1]*b[1];}
struct Boundary {double radial, speed_second, density, effort_rate;};
inline Boundary boundary(double R,double z,double zdot,double angular_costate){
 if(R<=0)throw std::invalid_argument("radius");
 double A=1/(R*R)-z*z/R;
 double zdd=-angular_costate/(2*R)-2*A*z/R;
 double mu=(-4*A/(R*R*R)-6*zdot*zdot/R-8*z*zdd/R-2*A*z*z/(R*R))/R;
 return {A,zdd,mu,A*A+zdot*zdot};
}
struct Tangent {bool feasible;double abs_min,abs_max;bool coast_allowed;};
inline Tangent tangent_range(double A,double amin,double amax){
 if(amin<0||amax<amin)throw std::invalid_argument("acceleration bounds");
 if(std::abs(A)>amax)return {false,0,0,false};
 return {true,std::sqrt(std::max(0.,amin*amin-A*A)),std::sqrt(std::max(0.,amax*amax-A*A)),A==0};
}
// Bounds in exact arithmetic; callers must inflate for floating point and trajectory error.
inline double squared_radius_lower(Curve p){
 const double b3[]={1,3,3,1},b6[]={1,6,15,20,15,6,1};
 double result=INFINITY;
 for(int k=0;k<=6;++k){double c=0;for(int i=0;i<4;++i){int j=k-i;
  if(j>=0&&j<4)c+=b3[i]*b3[j]*dot(p[i],p[j])/b6[k];}result=std::min(result,c);}
 return result;
}
inline std::array<Curve,2> split(Curve p){
 auto mid=[](V a,V b){return V{(a[0]+b[0])/2,(a[1]+b[1])/2};};
 V a=mid(p[0],p[1]),b=mid(p[1],p[2]),c=mid(p[2],p[3]);
 V d=mid(a,b),e=mid(b,c),f=mid(d,e);
 return {Curve{p[0],a,d,f},Curve{f,e,c,p[3]}};
}
// 1 safe, -1 violating, 0 unresolved. E is a certified uniform position error.
// This floating-point reference needs outward rounding for a formal machine certificate.
inline int audit(Curve p,double R,double E,int depth=12){
 if(squared_radius_lower(p)>=(R+E)*(R+E))return 1;
 auto halves=split(p);
 for(V x:{p[0],halves[0][3],p[3]})if(std::sqrt(dot(x,x))+E<R)return -1;
 if(depth==0)return 0;
 int a=audit(halves[0],R,E,depth-1),b=audit(halves[1],R,E,depth-1);
 if(a==-1||b==-1)return -1;
 return a==1&&b==1?1:0;
}
}
