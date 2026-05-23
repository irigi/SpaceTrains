# SpaceTrains — Game Design

## Concept

SpaceTrains is a **self-playing Solar System economy simulation**. The player is an observer: they watch an autonomous ecosystem of space stations and freighters operate, trade, and travel without direct control. The experience is about understanding the system — seeing why a route is efficient, why a station runs short, how launch windows cluster departures.

## Solar System Simulation

The game models the inner Solar System with space stations orbiting planets. Stations receive regular goods from their parent planet (via economy production rules) and trade surplus goods with stations elsewhere in the system. Some stations are subsidized science outposts that are net consumers — they receive money for doing science but depend on imports for materials and supplies.

The celestial mechanics are circular Keplerian orbits. Bodies are data-driven (loaded from CSV), so the Solar System content can be extended without code changes.

## Ships and Propulsion

Between stations travel autonomous cargo ships. Each ship tracks:
- Current fuel / propellant level
- Active mission: origin, destination, cargo manifest
- Mission phase: idle, awaiting departure, in transit, refueling, stranded

Ships choose missions greedily: they prefer routes with the highest cargo throughput (units per day), subject to fuel availability.

### Two Propulsion Modes

**Chemical propulsion (Kepler mode):**  
Ships with chemical engines perform impulsive maneuvers. Interplanetary transfers follow Hohmann trajectories. The ship waits for the correct synodic phase angle before departure (launch window). Same-parent local transfers use direct cruise burns.

**Electric ion propulsion (VariableISP mode):**  
Ships with ion engines use constant-power optimal trajectories. The engine runs continuously, trading exhaust velocity for thrust moment-by-moment (variable specific impulse). The governing physics is a Pontryagin optimal control problem; solutions are precomputed in a trajectory atlas indexed by:
- **ρ (rho)** — radius ratio r_destination / r_origin
- **κ (kappa)** — dimensionless ship capability: κ = 2P(1/m_dry − 1/m₀)(r₀²·⁵/μ¹·⁵)
- **θ (theta)** — total heliocentric angular displacement during transfer

The atlas covers a wide range of ship capabilities and transfer geometries. At mission planning time, the planner searches the theta grid for the departure phase that minimizes total trip time (wait + transfer). A ship will wait for a launch window if departing later means arriving sooner.

## Ship Tech Levels

A ship class is characterized by two fundamental tech-level parameters:

- **α (alpha)** — specific engine power [W per kg of dry mass]. This determines the engine's power-to-weight ratio. Higher α means more capable engines (higher technology level).
- **ε (epsilon)** — fuel mass fraction: propellant / total wet mass. This determines how much of the ship is fuel tank.

Together, α and ε determine κ for the VariableISP atlas lookup:
```
P = α × m_dry
m₀ = m_dry + m_propellant = m_dry / (1 − ε)
κ = 2P(1/m_dry − 1/m₀) × (1 AU)^2.5 / μ_sun^1.5
```

Ships within a class can vary slightly in α (engine manufacturing variation, ±10–20%) and more substantially in total size (mass scaling), while preserving the specific parameters. Scaling size while keeping α and ε constant leaves κ unchanged — same trajectory performance, different cargo capacity.

## Economy

Each station has an economy profile that drives production and consumption of commodities (food, water, oxygen, metals, fuel, electronics, etc.). Stations with a surplus become supply sources; stations with a deficit become demand sinks. Ships match supply to demand and earn throughput by carrying cargo.

Science stations are net consumers — they draw resources but provide no direct commodity output. Their value to the network is indirect (lore/backstory: they produce scientific knowledge valued by factions).

## Development Philosophy

**Headless-first:** The simulation runs fully without Godot graphics. Physics correctness, economy balance, and ship behavior are verified through the headless executable and its debug output before any visual work is done. This allows rapid iteration on simulation parameters.

**Data-driven:** The Solar System, station layouts, ship classes, and commodity flows are all defined in CSV files. Tuning the simulation does not require code changes.

**Observer sandbox:** The player has no direct control over ships or economy. The gameplay is watching the system and understanding it. Time controls (pause, fast-forward) allow observing the simulation at human scale or skipping ahead.

**Trajectory fidelity:** Electric ship trajectories are computed from a precomputed atlas of optimal solutions, not approximated by straight lines or Hohmann analogs. Transfer times and fuel consumption match physics-based expectations from the atlas.
