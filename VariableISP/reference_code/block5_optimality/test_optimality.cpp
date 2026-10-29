#include "optimality.hpp"
#include <cassert>
#include <iostream>
using namespace block5;
bool near(double a,double b,double atol=1e-11){return std::abs(a-b)<atol;}
State circle(double r,double th){return {r*std::cos(th),r*std::sin(th),-std::sin(th)/std::sqrt(r),std::cos(th)/std::sqrt(r)};}
#include "fixture_tube.inc"
int main(){
  auto g=gains(1,0);assert(near(g.kr2,1./3)&&near(g.kv2,1)&&near(g.ar,1./12)&&near(g.av,.5));
  // Two-revolution coast: its cost and lower bound are exactly zero.
  const double pi=std::acos(-1.);auto coast=lower(circle(1,0),circle(1,4*pi),4*pi,.1,0);
  assert(coast.combined==0.);
  auto F=jacobi_matrix({1,0},{2,0});
  assert(F[2*8+0]==2&&F[3*8+1]==-1);
  assert(F[4*8+0]==12&&F[5*8+1]==-6); // -K, not +K.
  // Budget branch: fixed regression inputs from the Python reconstruction.
  auto fixture=fixture_tube();
  assert(fixture.diagnostic_pass&&fixture.gamma>.89&&fixture.containment_margin>.035);
  assert(fixture.gap<1e-10);
  double h=.712023798759808,rho=1.01,th=.005+std::pow(rho,-1.5)*h;
  auto lb=lower(circle(1,0),circle(rho,th),h,.1,.0032);
  assert(near(lb.combined,1.7723015009145e-6,1e-16));
  // Synthetic conjugate fixture: characteristic root and explicit negative mode.
  double beta=4.7300407448627,tc=beta/std::pow(600.,.25);
  assert(near(tc,.9557120662747031));
  assert(std::abs(std::cosh(beta)*std::cos(beta)-1)<1e-11);
  assert(8*std::pow(pi,4)-900<0);
  bool rejected=false;try{(void)gains(1,101);}catch(const std::invalid_argument&){rejected=true;}
  assert(rejected);
  auto bad=tube(1,1,1,.1,10);assert(!bad.diagnostic_pass&&bad.lower==0);
  std::cout<<"Block 5 C++ regression checks passed\n";
}
