#include "perihelion.hpp"
#include <cassert>
#include <iostream>
int main(){using namespace perihelion;
 auto coast=boundary(.1,std::sqrt(10.),0,0);
 assert(std::abs(coast.radial)<1e-12&&coast.effort_rate<1e-20);
 auto fast=boundary(.1,3.2,0,30.72);
 assert(std::abs(fast.radial+2.4)<1e-12);
 assert(std::abs(fast.speed_second)<1e-10);
 assert(std::abs(fast.density-145152)<1e-6);
 auto tr=tangent_range(fast.radial,.8,4);
 assert(tr.feasible&&tr.abs_min==0&&std::abs(tr.abs_max-3.2)<1e-12);
 auto slow=boundary(.1,3,0,-120);
 assert(slow.density<0&&!tangent_range(slow.radial,.8,4).feasible);
 Curve safe={V{-.02,.101},V{-.02/3,.101},V{.02/3,.101},V{.02,.101}};
 Curve hidden={V{.12,0},V{.08,0},V{.08,0},V{.12,0}};
 assert(audit(safe,.1,1e-5)==1);assert(audit(hidden,.1,1e-5)==-1);
 std::cout<<"boundary dynamics, band intersection, and intersample audit tests passed\n";
}
