#pragma once
// O(1) evaluation from stored atlas values. No trajectory integration here.
// H = p.f - |u|^2 (maximized), u=p_v/2, dimensionless effort integral |u|^2.
#include <array>
#include <cmath>
#include <stdexcept>

namespace kepler_sens {
using V2=std::array<double,2>;
inline double sq(const V2&a){return a[0]*a[0]+a[1]*a[1];}
struct AtlasData {
  double rho,J,H,L;
  V2 u0,uf;
  // Offline sensitivities: geometry J_ThetaTheta, moving L_h and moving F_hh.
  double J_ThetaTheta,L_h_moving,F_hh;
};
struct Derivatives {
  double J_Theta,J_h_geometry;
  double departure_fixed_arrival,arrival_fixed_departure;
  double departure_fixed_duration,duration_fixed_departure;
  double F_dd,F_dh,F_hh;
  double initial_identity_residual,final_identity_residual;
};
inline Derivatives evaluate(const AtlasData&a){
  if(!(a.rho>0))throw std::invalid_argument("rho must be positive");
  const double nf=1/(a.rho*std::sqrt(a.rho)),alpha=nf-1;
  const double s0=sq(a.u0),sf=sq(a.uf);
  return {a.L,-a.H,s0,-sf,alpha*a.L,-sf,
          alpha*alpha*a.J_ThetaTheta,alpha*a.L_h_moving,a.F_hh,
          a.H-a.L-s0,a.H-nf*a.L-sf};
}
inline double active_time_budget_slope(const V2&uf){
  double q=sq(uf);if(q==0)throw std::domain_error("singular budget-time slope");
  return -1/q;
}
struct Fuel {double mass_ratio,propellant_ratio,propellant_time_derivative;};
// Ratios to fixed initial mass; chi=2P/(m0 J0).
inline Fuel fixed_initial_mass(double J,double J_h,double chi){
  if(!(chi>0)||J<0)throw std::invalid_argument("fuel arguments");
  const double den=chi+J;
  return {chi/den,J/den,chi/(den*den)*J_h};
}
// Ratios to fixed arrival/dry mass; chi_d=2P/(md J0), P fixed across loads.
inline Fuel fixed_arrival_mass(double J,double J_h,double chi_d){
  if(!(chi_d>0)||J<0||!(J<chi_d))throw std::domain_error("no finite admissible load");
  const double den=chi_d-J;
  return {chi_d/den,J/den,chi_d/(den*den)*J_h};
}
// Same physical units on both legs. Weights 1 for total effort, 1/(2P) for
// total inverse-mass increment when power differs and no refueling occurs.
inline double stop_time_gradient(const V2&incoming_final,const V2&outgoing_initial,
                                double incoming_weight=1,double outgoing_weight=1){
  return outgoing_weight*sq(outgoing_initial)-incoming_weight*sq(incoming_final);
}
} // namespace kepler_sens
