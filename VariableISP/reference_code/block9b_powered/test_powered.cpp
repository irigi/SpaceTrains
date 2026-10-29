#include "powered_flyby.hpp"
#include <cassert>
#include <iostream>
int main(){using namespace powered_flyby;
 V x{1,0},v{-1.6,.5};V wi=asymptote(x,v,true);
 assert(std::abs(wi[0]+.8307692307692309)<1e-12&&std::abs(wi[1]-.3461538461538461)<1e-12);
 V xf{-.08006201534445251,-.996789884428501},vf{.3726109639082036,-1.661578880295951};
 V wo=asymptote(xf,vf,false);double sout=std::sqrt(dot(wo,wo));
 assert(std::abs(sout-.9485163709025519)<2e-11);
 double work=energy(xf,vf)-energy(x,v);
 assert(std::abs(work-.044841652940325404)<1e-10);
 double lb=energy_effort_lower(.9,sout,.1,.9649409729295894);
 assert(std::abs(lb-9.992084168710172e-5)<1e-12&&lb<.00038597638917183533);
 auto c=soi_crossing(V{0,1},V{.5,-2},V{0,std::sqrt(.1)},V{-.01+.009851853368415734,-.01+.0009851853368415735},V{.3,.4},V{.1,-.2});
 assert(std::abs(c.multiplier+.0018251849273693253)<1e-14);
 assert(std::abs(c.hamiltonian_jump-(.44532764249922147-.44590481665137105))<1e-14);
 bool threw=false;try{asymptote(V{1,0},V{0,1},true);}catch(const std::domain_error&){threw=true;}assert(threw);
 std::cout<<"asymptote, powered work, resource lower bound and SOI jump tests passed\n";
}
