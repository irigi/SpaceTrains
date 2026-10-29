#include "flyby.hpp"
#include <cassert>
#include <iostream>
int main(){using namespace flyby;
 V w{1,0};auto d=limit(w,.01,.01);double pi=std::acos(-1.);
 assert(std::abs(d.angle-pi/3)<1e-14);assert(std::abs(d.max_kick-1)<1e-14);
 M m=fixed_radius_jacobian(w,.01,.01,1);
 assert(std::abs(m[0][0]-1.5)<1e-14&&std::abs(m[1][0]-1/(2*std::sqrt(3.)))<1e-14);
 assert(std::abs(m[0][0]*m[1][1]-m[0][1]*m[1][0]-1)<1e-14);
 for(V x:{V{1,0},V{.3,.4},V{-1.2,.7}})for(int side:{-1,1}){
  M jac=fixed_radius_jacobian(x,.01,.01,side);
  for(int j=0;j<2;++j){V a=x,b=x;a[j]+=1e-6;b[j]-=1e-6;
   V fa=rotate(a,side*limit(a,.01,.01).angle),fb=rotate(b,side*limit(b,.01,.01).angle);
   for(int i=0;i<2;++i)assert(std::abs((fa[i]-fb[i])/2e-6-jac[i][j])<5e-9);}}
 V wp=rotate(w,d.angle),pvp=J(wp),pvm=transpose(m,pvp),prm{-1./3,0},prp{0,0},vp{0,1},ap{-1,0};
 double Hm=dot(prm,add(vp,w))+dot(pvm,ap)+dot(pvm,pvm)/4;
 double Hp=dot(pvp,ap)+dot(pvp,pvp)/4;
 auto g=objective_gradient(prm,prp,pvm,pvp,Hm,Hp,vp,ap,wp,d.angle);
 assert(std::abs(g.epoch)<1e-13&&std::abs(g.angle+1)<1e-13);
 assert(std::abs(g.incoming_excess[0]-d.gradient[0])<1e-13);
 auto bad=check(V{.5,0},rotate(V{.5,0},.6),3e-6,5e-5,1e-14);
 assert(bad.equal_speed&&!bad.allowed_turn&&bad.required_radius<5e-5);
 auto speed=check(V{1,0},V{1.01,0},.01,.01,1e-12);assert(!speed.equal_speed);
 std::cout<<"turn, active-bound Jacobian, event KKT and rejection tests passed\n";
}
