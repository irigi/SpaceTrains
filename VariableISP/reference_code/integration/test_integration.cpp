#include "block2_sensitivities/kepler_sensitivities.hpp"
#include "block3_exhaust_band/band.hpp"
#include "block9b_powered/powered_flyby.hpp"
#include "block6_escape_capture/escape.hpp"
#include <cassert>
#include <iostream>
#include <random>
int main(){
    // Band selector reduces to the ideal effort gain when both limits are inactive.
    for(double a : {.02,.05,.1}){
        auto s=band::select(2*a,1.,.5,2.,50.);
        assert(std::abs(s.acceleration-a)<1e-15);
        assert(std::abs(s.gain-a*a)<1e-15);
        assert(s.costate_rate==0);
    }
    double m=1000.,P=1e6;
    for(auto pair : {std::array<double,2>{20.,2.},std::array<double,2>{40.,5.}}){
        auto f=kepler_sens::fixed_initial_mass(pair[0]/pair[1],0.,2*P/(m*pair[1]));
        m*=f.mass_ratio;
    }
    assert(std::abs(m-1/(.001+60./2e6))<3e-13);
    // The independent Block 9B and Block 6 SOI APIs must agree.
    std::mt19937 gen(20261029);std::uniform_real_distribution<double> d(-1.,1.);
    for(int i=0;i<256;++i){
        std::array<double,2> x{1.,.2},v{(i%2?1.:-1.),d(gen)*.2};
        std::array<double,2> V{d(gen),d(gen)},dg{d(gen),d(gen)};
        std::array<double,2> pv{d(gen),d(gen)},pr{d(gen),d(gen)};
        auto a=powered_flyby::soi_crossing(x,v,V,dg,pv,pr);
        auto b=escape_capture::soi_jump(x,v,V,dg,pv,pr);
        assert(std::abs(a.multiplier-b.multiplier)<1e-14);
        assert(std::abs(a.hamiltonian_jump-b.hamiltonian_jump)<1e-14);
        for(int k=0;k<2;++k)assert(std::abs(a.p_position_after[k]-b.pr_after[k])<1e-14);
        auto grad=escape_capture::interface_gradient(x,v,V,{0,0},pr,pv,0,b.pr_after,pv,b.hamiltonian_jump);
        for(double g:grad)assert(std::abs(g)<2e-14);
    }
    // Same positive-energy annulus data: Block 6 contains the Block 9B necessary bounds.
    auto e=powered_flyby::energy_effort_lower(.4,.7,.2,2.);
    auto j=powered_flyby::angular_effort_lower(.1,.3,1.,2.);
    auto all=escape_capture::necessary_effort(.4*.4/2,.7*.7/2,.1,.3,.2,1.,2.,.1,5.);
    assert(all>=e-1e-15 && all>=j-1e-15);
    std::cout<<"PASS: shared gain, mass map, 256 SOI API matches, bound compatibility\n";
}
