#include "escape.hpp"
#include <cassert>
#include <iostream>
using namespace escape_capture;
void near(double a,double b,double e=1e-10){assert(std::abs(a-b)<=e);}
int main(){
 auto no=minimum_effort(.5,480,.01,10,50);assert(!no.feasible);
 auto a=minimum_effort(.5,490,.01,10,50);near(a.effort,.0005102480821346535,1e-13);near(a.burn_time,490,1e-6);
 auto b=minimum_effort(.5,1000,.01,10,50);near(b.effort,.00025,1e-13);
 auto c=minimum_effort(.5,2480,.01,10,50);near(c.effort,.00010080648347863767,1e-13);
 auto d=minimum_effort(.5,3000,.01,10,50);near(d.effort,.00010050167084168059,1e-14);near(d.coast_time,512.4584372920131,1e-8);
 auto budget=minimum_time(.5,.0002,.01,10,50);near(budget.burn_time,1250,1e-7);
 auto cap=minimum_time(.5,.00001,.01,10,50);assert(!cap.feasible);
 auto zero=minimum_effort(0,10,.01,10,50);near(zero.effort,0);near(zero.coast_time,10);
 auto eq=minimum_effort(.5,10000,.01,30,30);assert(eq.feasible);
 auto s0=exact_shape(0),sf=exact_shape(10);near(s0.x[0],1);near(s0.v[1],1);
 near(sf.x[0],1.134648741852905);near(sf.v[1],-.43352347206626995);
 near(exact_effort(),.06335999205292772,1e-14);
 double amin=.06,amax=std::sqrt(181.)/150;
 assert((.9+exact_effort())/50<amin&&.9/10>amax);
 near(necessary_effort(-.5,.055,1,2,.8,4,10,.9,50),.015272821251812427,1e-13);
 V x{0,1},vrel{.5,-2},Vp{0,std::sqrt(.1)},Ap{-.01,0},pv{.3,.4},pr{.1,-.2};
 V dG{-.00014814663158427,-.00901481466315843};
 auto j=soi_jump(x,vrel,Vp,dG,pv,pr);near(j.multiplier,-.0018251849273693253,1e-14);
 auto grad=interface_gradient(x,vrel,Vp,Ap,pr,pv,.445904816651371,j.pr_after,pv,.445327642499221);
 for(double z:grad)near(z,0,1e-13);
 std::cout<<"Block 6 C++ checks passed\n";
}
