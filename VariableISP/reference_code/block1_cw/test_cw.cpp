#include "cw_planar.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <iomanip>
bool close(double a,double b){return std::abs(a-b)<=2e-12+2e-9*std::abs(b);}
int main(){
  cw::Vec z1{1e-5,0,0,0},z2{.01,-.02,.003,-.004};
  auto a=cw::correction_data(.01,z1);
  assert(close(a.cost,.0012000880025676134));
  assert(close(a.derivative,-.3600087997432371));
  assert(close(cw::game_time(z1,.0027001320030447355,.0003),.01));
  auto u=cw::correction(0,.01,a.lambda);
  assert(close(u[0],.6000240002838023));assert(close(u[1],.004000033334587273));
  const double h=cw::game_time(z2,.0032,.0008);
  assert(close(h,4.204601204168997));
  auto b=cw::correction_data(h,z2);
  assert(close(b.cost,.0008));assert(close(b.derivative,-.00035111334490092234));
  assert(std::isinf(cw::game_time(z2,.0032,.0032)));
  assert(cw::game_time({0,0,0,0},0,0)==0);
  auto c=cw::certificate(.2,{.05,0,0,0},{0,0,0,0},1e-5,.25);
  assert(c.tube_valid);assert(close(c.tube_lhs,.18558867549181354));
  assert(close(c.required_bp,5.440235820667281));
  // Small-h scaled Gramian tends to [[I/3,I/2],[I/2,I]].
  auto w=cw::scaled_gramian(1e-8);
  assert(close(w[0][0],1./3));assert(close(w[0][2],.5));assert(close(w[2][2],1.));
  volatile double sink=0;
  auto start=std::chrono::steady_clock::now();
  for(int i=0;i<100000;++i)sink+=cw::correction_data(.2+(i%1000)*.005,z2).cost;
  auto end=std::chrono::steady_clock::now();
  double us=std::chrono::duration<double,std::micro>(end-start).count()/100000;
  start=std::chrono::steady_clock::now();
  for(int i=0;i<1000;++i)sink+=cw::game_time(z2,.0032*(1+i*1e-5),.0008);
  end=std::chrono::steady_clock::now();
  double roots=std::chrono::duration<double,std::micro>(end-start).count()/1000;
  std::cout<<std::setprecision(16)<<"all tests passed\nH="<<h<<"\nC_eval_us="<<us
           <<"\nroot_us="<<roots<<"\n";
}
