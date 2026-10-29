#include "kepler_sensitivities.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <iomanip>
bool close(double a,double b){return std::abs(a-b)<=2e-12+2e-9*std::abs(b);}
int main(){
  using namespace kepler_sens;
  AtlasData coast{1,0,0,0,{0,0},{0,0},26.339557333915135,0,0};
  auto d0=evaluate(coast);
  assert(d0.duration_fixed_departure==0&&d0.departure_fixed_duration==0&&d0.F_hh==0);
  AtlasData a{1.01,.0032,-.005299294556583272,-.019268535328496705,
    {.11403418474318612,.03107161859109259},
    {-.1081697178839428,-.044531966545096655},
    70.99411843752485,-.4464008665372112,.08094313528993778};
  auto d=evaluate(a);
  assert(close(d.J_h_geometry,.005299294556583272));
  assert(close(d.duration_fixed_departure,-.013683783911465384));
  assert(close(d.departure_fixed_arrival,.013969240771913433));
  assert(close(d.departure_fixed_duration,.0002854568604480495));
  assert(close(d.F_dd,.015581380507851765));
  assert(close(d.F_dh,.006613278471378538));
  assert(std::abs(d.initial_identity_residual)<1e-12&&std::abs(d.final_identity_residual)<1e-12);
  assert(close(active_time_budget_slope(a.uf),-73.07920137222563));
  auto f=fixed_initial_mass(a.J,d.duration_fixed_departure,.02);
  assert(close(f.mass_ratio,.8620689655172666));
  assert(close(f.propellant_ratio,.1379310344827334));
  assert(close(f.propellant_time_derivative,-.5084640276258243));
  auto g=fixed_arrival_mass(a.J,d.duration_fixed_departure,.0232);
  assert(close(g.mass_ratio,1.1599999999999606));
  assert(close(g.propellant_time_derivative,-.7936594668649385));
  assert(close(stop_time_gradient(a.uf,{.1,.05}),-.0011837839114653818));
  assert(close(stop_time_gradient(a.uf,a.uf),0));
  bool threw=false;try{fixed_arrival_mass(.02,0,.02);}catch(const std::domain_error&){threw=true;}assert(threw);
  threw=false;try{active_time_budget_slope({0,0});}catch(const std::domain_error&){threw=true;}assert(threw);
  volatile double sink=0;
  auto start=std::chrono::steady_clock::now();
  for(int i=0;i<1000000;++i){a.rho=1.005+(i%1000)*1e-5;sink+=evaluate(a).departure_fixed_duration;}
  double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/1e6;
  std::cout<<std::setprecision(16)<<"all tests passed\nevaluation_us="<<us<<"\n";
}
