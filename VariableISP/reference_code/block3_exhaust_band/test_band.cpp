#include "band.hpp"
#include <cassert>
#include <iostream>
int main(){
 double svals[]={.05,.15,.4,2}; int modes[]={0,1,2,3};
 double acc[]={0,.1,.2,.5}; double cd[]={0,-.025,0,2.5};
 for(int i=0;i<4;++i){auto z=band::select(svals[i],1,.2,.4,2);
  assert(z.mode==modes[i]);assert(std::abs(z.acceleration-acc[i])<1e-14);assert(std::abs(z.costate_rate-cd[i])<1e-14);}
 // Compare with a dense independent pointwise maximization, including nonpositive C.
 for(int i=0;i<101;++i)for(int j=0;j<81;++j){double s=i*.03,C=-1+j*.05;
  auto z=band::select(s,C,.2,.4,2);double best=0;
  for(int k=0;k<=2000;++k){double a=.1+.4*k/2000;best=std::max(best,s*a-C*a*a);}
  assert(z.gain+1e-12>=best);assert(z.gain-best<1e-7);}
 assert(!band::admissible(.05,.2,.4,2));assert(band::admissible(0,.2,.4,2));
 assert(band::interval_admissible(.12,.4,.2,.21,.4,2));
 double lm=band::boundary_lambda_m(.4,.6,.2,1,.5,1,.1);
 assert(std::abs(lm-(.4+.8*(1/.9-1)+.2*std::log(.9)))<1e-14);
 std::cout<<"all band selector and boundary-map tests passed\n";
}
