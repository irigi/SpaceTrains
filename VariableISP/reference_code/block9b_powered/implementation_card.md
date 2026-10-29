# Block 9B — finite powered flyby

Companion to Block 9's unpowered point event. Planar, translating nonrotating planet frame; central planetary gravity inside a fixed SOI, Sun gravity outside. Solar tides inside are omitted. No impulse occurs at an SOI crossing. Useful jet power P is constant; the finite exhaust model is full power/on–off. Arbitrary throttling requires its own consistent mass-flow law.

## 1. Units and inputs

Choose L = SOI radius, T_p = sqrt(L³/μ_p), V_p = sqrt(μ_p/L), A_p = μ_p/L², J_p = μ_p^(3/2)/L^(5/2). All local formulas below use these units unless identified as common-frame formulas.

- x' = v, v' = −x/|x|³ + u, b' = −|u|².
- ε = minimum planetocentric radius / L; ε ≤ |x| ≤ 1.
- Entry |x|=1, x·v<0; first exit |x|=1, x·v>0.
- b_i is remaining effort budget at encounter i, initially κ_p. χ_d = 2P/(m_d J_p); w = χ_d − b = 2P/(m J_p).
- ν₋ = 50 km/s / V_p, ν₊ = 250 km/s / V_p; ratio 5. Allowed thrust is zero or w/ν₊ ≤ |u| ≤ w/ν₋, with coast only at b=0.
- If χ=w_entry and effort consumed is I, m_exit/m_entry = χ/(χ+I), b_exit=b_entry−I.

The endpoint radius ratio is ρ=1 for equal-radius SOI crossings. Θ is the unwrapped position-angle difference arg(x_exit)−arg(x_entry); it is **not** the scattering angle Δ between excess velocities. Unlike circular rendezvous, (ρ, Θ, h) does not specify the endpoints: velocities or equivalent scattering data are additional arguments. Distinct winding branches remain distinct.

For Sun atlas length R₀, λ=L/R₀ and γ=μ_p/μ_Sun: T_p/T₀=λ^(3/2)/sqrt(γ), V_p/V₀=sqrt(γ/λ), J_p/J₀=γ^(3/2)/λ^(5/2). Multiply local effort and duration by these factors before adding to Sun-unit legs. Budgets and costates must use the same scale at a junction.

## 2. Constant-cost runtime formulas

Let ℛ(x,y)=(−y,x), j=x×v, E=|v|²/2−1/|x|, s=sqrt(2E), e_vec=(|v|²−1/|x|)x−(x·v)v, e=|e_vec|, n=e_vec/e. For E>0:

    incoming excess = (s/e) ( n + j s ℛn)
    outgoing excess = (s/e) (−n + j s ℛn)

Use the incoming expression on the entry state and outgoing expression on the exit state. These are hypothetical ballistic planetary tails, not the actual powered heliocentric legs. Reject hyperbolic labeling for E≤0. No equality of the two excess speeds is imposed.

Exact checks:

    (s_out² − s_in²)/2 = integral(v·u dt)
    j_out − j_in = integral(x×u dt)

For a proposed duration h>0 and safety floor ε>0, define

    dW = sqrt(s_out² + 2/ε) − sqrt(s_in² + 2/ε)
    J_lower = max(dW², (j_out−j_in)²) / h

Reject if J_lower>b_i (within numerical tolerance). Also reject if either |dW| or |j_out−j_in| exceeds ν₊ log1p(b_i/χ), where χ=w_entry. These are necessary conditions only; they do not certify geometry or optimality. Never add the two lower bounds.

## 3. Control and shooting equations

Use the maximizing Hamiltonian H=p_x·v+p_v·g−ℓ₀+p_v·u−C|u|², g=−x/|x|³, C=p_b+ℓ₂. Fixed-time effort has (ℓ₀,ℓ₂)=(0,1); direct minimum time has (1,0).

With s_c=|p_v|, a_low=w/ν₊, a_high=w/ν₋, form a=clip(s_c/(2C),a_low,a_high) for C>0; for C≤0 take a=a_high. Compare s_c a−C a² with zero. Positive: burn toward p_v; negative: coast; zero: switching tie. When s_c=0 and a burn wins, direction is undetermined by this first-order selector. When C=s_c=0 all admissible magnitudes tie.

    p_x' = −Dg(x)^T p_v
    p_v' = −p_x
    C' = (s_c−2Ca)/ν on an active a=w/ν boundary
    C' = 0 on interior/coast arcs

Apply Block 4's radial path-constraint conditions at |x|=ε and dry-mass conditions at b=0. In the ideal engine model, u=p_v/(2C), C>0 constant on a regular unconstrained phase. A nontrivial powered extremal has no positive-length coast interval on that phase. This does **not** exclude coast intervals in the finite-band model.

## 4. Two SOI junctions, in one common coordinate/unit system

Let R_p,V_p,A_p be the planet ephemeris; x=X−R_p and v=V−V_p. Use G_S=−μ_S X/|X|³ outside and G_P=A_p−μ_p x/|x|³ inside. The translating-origin acceleration A_p is required for the local pure-Kepler approximation.

At each transverse crossing, ΔG=G_after−G_before and

    ζ = −(p_V·ΔG)/(x·v)
    p_X_after = p_X_before + ζ x
    p_V_after = p_V_before
    p_b_after = p_b_before
    H_after − H_before = ζ x·V_p

ζ has no sign restriction. These formulas assume unchanged engine parameters and a unique regular selector. States and mass are continuous. Thrust is then continuous; the patched gravitational acceleration jumps. A near-zero x·v flags an ill-conditioned grazing event, not a safe denominator to clamp silently. Coincident constraints need additional junction conditions.

For given continuous control, state saltation is S=I+(f_after−f_before)nᵀ/(x·v), n=(x,0,0). It satisfies p_before=Sᵀp_after. This alone is not the full state–costate shooting Jacobian.

Before matching, physical effort covectors are p_x,phys=(J_p/L)p_x, p_v,phys=(J_p/V_p)p_v, H_phys=(J_p/T_p)H, and p_B,phys=p_b if B=J_p b. The relative Hamiltonian is H_rel=H_global−p_X·V_p−p_V·A_p.

## 5. Numerical fixtures

Use approximately 2e−9 absolute tolerance on integrated positions, velocities, times and minimum radius; 2e−10 on integrated work identities; 2e−13 on algebraic junction fixtures. `test_cases.json` contains more digits. `validate.py` regenerates them independently through DOP853 integration and event detection. The C++ test checks the cheap formulas against stored values.

### Case 1: ballistic reference

μ_p=1, SOI=1, ε=0.1; x_in=(1,0), v_in=(−1.6,0.5), u=0.

| Quantity | Expected |
|---|---:|
| SOI-to-SOI duration | 0.969948875603578 |
| Minimum radius | 0.119241493794037 |
| Exit position | (−0.064449064446662, −0.997920997921153) |
| Exit velocity | (0.395841995846036, −1.628898128894004) |
| Incoming excess | (−0.830769230769231, 0.346153846153846) |
| Outgoing excess | (0.291891891893201, −0.851351351344889) |
| Both excess speeds | 0.9 |
| Signed scattering angle Δ | 2.295884801327616 rad |
| Effort | 0 |

An independent analytic check uses a_h=1/s², e=sqrt(1+j²s²), F=acosh((1/a_h+1)/e), h=2 sqrt(a_h³)(e sinh(F)−F), with s=0.9,j=0.5.

### Case 2: feasible powered pass, not an optimum

Same initial state; u=0.02 v/|v| throughout. χ=w_in=0.4, κ_p=b_in=0.0032, χ_d=0.4032, ν₋=10, ν₊=50, ε=0.1. Thus the exhaust-speed ratio is the game's ratio; these are synthetic planetary units, not an Earth calibration. Setting V_p=5 km/s makes the band exactly 50–250 km/s.

| Quantity | Expected |
|---|---:|
| Duration | 0.964940972929589 |
| Minimum radius | 0.119968327339380 |
| Exit position | (−0.080062015344453, −0.996789884428501) |
| Exit velocity | (0.372610963908204, −1.661578880295951) |
| Outgoing excess | (0.286428635697124, −0.904235556988792) |
| Outgoing excess speed | 0.948516370902552 |
| Signed scattering angle Δ | 2.272351586431333 rad |
| Effort J_F | 0.000385976389172 |
| Remaining b | 0.002814023610828 |
| Fuel fraction of entry mass | 0.000964010759449 |
| Energy gain ∫v·u | 0.044841652940325 |
| Angular momentum gain | 0.004444193461209 |
| Energy-based effort lower bound | 0.000099920841687 |

The band is satisfied throughout: (0.4+J_F)/50 < 0.02 < 0.4/10. About 11.51% of encounter time/effort lies inside r=0.2, supplying about 18.82% of the energy gain. This illustrates the finite-thrust Oberth effect. The exit differs from Case 1: the shorter local duration is not proof of a better solution to the same fixed-endpoint mission.

### Case 3: moving SOI entry adjoint jump

These are **common global units**, different from Cases 1–2. μ_S=1, μ_p=0.01, SOI=1; R_p=(10,0), V_p=(0,sqrt(0.1)), A_p=(−0.01,0). At entry x=(0,1), v=(0.5,−2), p_V=(0.3,0.4), p_X_before=(0.1,−0.2).

| Quantity | Expected |
|---|---:|
| G_before | (−0.009851853368416, −0.000985185336842) |
| G_after | (−0.01, −0.01) |
| ζ | −0.001825184927369 |
| p_X_after | (0.1, −0.201825184927369) |
| H_before, ideal C=1 | 0.445904816651371 |
| H_after, ideal C=1 | 0.445327642499221 |

Check ΔH−ζ x·V_p=0. For four-state order (X_x,X_y,V_x,V_y), S is the identity plus S[2,1]=0.0000740733157921 and S[3,1]=0.0045074073315792 (zero-based indexing). Check p_before=Sᵀp_after.

## 6. Planner integration and limits

Optimize exterior legs and the finite encounter together: entry/exit dates, SOI positions, velocities, mass and duration allocation. Add physical efforts J_in+J_p J_F+J_out, then solve over total flight time against the resource budget. For a local fixed-state-endpoint action, ∂J_F*/∂h=−H_F on a smooth branch; this is not automatically −|u_exit|². Global optimized timing uses the original endpoint sensitivities with stationary interfaces accounted for.

Use offline local action branches with interpolation/derivatives for runtime planning. `powered_flyby.hpp` contains O(1) asymptote, rejection-bound and interface formulas; it is not a complete powered-flyby optimizer. No global optimality, branch completeness or few-millisecond new boundary-value solve is claimed. Instantaneous impulses have divergent quadratic effort and are not a replacement for the finite segment. Infinity-to-infinity labels simplify symmetry, but the corresponding literal elapsed time is infinite.

Build the algebraic fixture check with:

```sh
g++ -std=c++17 -O3 -Wall -Wextra -pedantic test_powered.cpp -o test_powered
./test_powered
python3 validate.py
```
