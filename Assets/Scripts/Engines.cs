using System.Collections.Generic;
using System;
using UnityEngine;

public class Resource
{
    public Resource(double mass_in) { mass = mass_in; }
    protected double mass;
    public virtual double Mass() { return mass; }
}

abstract public class Fuel 
{
    public Fuel()
    {
        requiredCombinations = new List<FuelResource>();
        requiredRatios = new List<double>();
        isp_mps = 0;
    }

    public double AmountKg()
    {
        double minAmountByRatio = 1e60;
        for (int i = 0; i < requiredCombinations.Count; ++i)
        {
            double amountByRatio = requiredCombinations[i].amountKg / requiredRatios[i];
            if (amountByRatio < minAmountByRatio) { minAmountByRatio = amountByRatio; }
        }

        double amountKg = 0;
        for (int i = 0; i < requiredCombinations.Count; ++i)
        {
            amountKg += minAmountByRatio * requiredRatios[i];
        }

        return amountKg;
    }

    public abstract double IspMps(double powerGW);

    public void BurnFuel(double amount)
    {
        double sumRatios = 0;
        foreach(double ratio in requiredRatios) { sumRatios += ratio; }

        for (int i = 0; i < requiredCombinations.Count; ++i)
        {
            requiredCombinations[i].amountKg -= amount * requiredRatios[i] / sumRatios;
        }
    }

    protected double isp_mps;
    public List<FuelResource> requiredCombinations { get; protected set; }
    public List<double> requiredRatios { get; protected set; }
}

public class FuelResource : Resource
{
    protected FuelResource(double fuel_density, double fuel_pressure, double fuel_temperature) : base(0)
    {
        density_kgm3 = fuel_density;
        pressure_bar = fuel_pressure;
        temperature_K = fuel_temperature;
        amountKg = 0;
    }
    public double amountKg;
    public double density_kgm3 { get; private set; }
    public double pressure_bar { get; private set; }
    public double temperature_K { get; private set; }     // this could vary, and with it the pressure. But let us assume it does not for now

    public override double Mass()
    {
        return amountKg;
    }
}

public class OxidizerResource : FuelResource
{
    protected OxidizerResource(double fuel_density, double fuel_pressure, double fuel_temperature) : base(fuel_density, fuel_pressure, fuel_temperature) { }
}

public class ChemicalFuelResource : FuelResource
{
    protected ChemicalFuelResource(double fuel_density, double fuel_pressure, double fuel_temperature, double oxidizer_ratio_mass, double isp_s) : base(fuel_density, fuel_pressure, fuel_temperature)
    {
        oxidizer_ratio = oxidizer_ratio_mass;
        isp_mps = isp_s * 9.81;
    }
    public double isp_mps { get; private set; }
    public double oxidizer_ratio { get; private set; }
}

public class LOXResource : OxidizerResource { public LOXResource() : base(1250.4, 6.89, 67.15) { } }

public class RP1Resource : ChemicalFuelResource { public RP1Resource() : base(813, 6.89, 295, 2.7, 370) { } }
public class MethaneResource : ChemicalFuelResource { public MethaneResource() : base(422, 6.89, 111.115, 3.7, 459) { } }
public class HydrogenResource : ChemicalFuelResource { public HydrogenResource() : base(71, 2, 20, 6, 532) { } }

public class ChemicalFuel : Fuel
{
    protected ChemicalFuel(ChemicalFuelResource fuelResource, OxidizerResource oxidizerResource)
    {
        requiredCombinations.Add(oxidizerResource);
        requiredCombinations.Add(fuelResource);
        requiredRatios.Add(fuelResource.oxidizer_ratio);
        requiredRatios.Add(1);
        isp_mps = fuelResource.isp_mps;
    }

    public override double IspMps(double powerGW)
    {
        return isp_mps;
    }
}

public class RP1Fuel : ChemicalFuel { public RP1Fuel(RP1Resource fuelResource, OxidizerResource oxidizerResource) : base(fuelResource, oxidizerResource) { } }
public class MethaneFuel : ChemicalFuel { public MethaneFuel(MethaneResource fuelResource, OxidizerResource oxidizerResource) : base(fuelResource, oxidizerResource) { } }
public class HydrogenFuel : ChemicalFuel { public HydrogenFuel(HydrogenResource fuelResource, OxidizerResource oxidizerResource) : base(fuelResource, oxidizerResource) { } }


public class FuelTank : Resource
{
    public FuelTank(double capacity_in, FuelResource resource_in, double dryMass) : base(dryMass)
    {
        capacity = capacity_in;
        resource = resource_in;
    }

    public double capacity { get; private set; }
    public FuelResource resource { get; private set; }
    public override double Mass()
    {
        return mass + resource.Mass();
    }
}


public class Engine : Resource
{
    protected Engine(Fuel fuel_in, double powerLimit_GW_in, double mass, Ship parent) : base(mass)
    {
        fuel = fuel_in;
        powerLimit_GW = powerLimit_GW_in;
        parentShip = parent;
    }

    public Ship parentShip;
    Fuel fuel;
    double powerLimit_GW;

    double FuelBurnedDeltaV(double shipStartMassIncludingFuel, double requestedDeltaV, double maxManeuverTime_s)
    {
        // integrated rocket equation
        double consumedFuel = shipStartMassIncludingFuel * (1 - Math.Exp(-requestedDeltaV / fuel.IspMps(powerLimit_GW)));
        return consumedFuel;
    }

    public void BurnFuelByDeltaV(double requestedDeltaV, double maxManeuverTime)
    {
        double amount = FuelBurnedDeltaV(parentShip.Mass(), Math.Abs(requestedDeltaV), maxManeuverTime);
        Debug.Log($"Burn fuel {amount:F0} for dv {requestedDeltaV:F0}");
        BurnFuelByAmount(amount);
    }

    public void BurnFuelByAmount(double amount) { fuel.BurnFuel(amount); }

    public bool CanProvideDeltaV(double requestedDeltaV, double maxManeuverTime)
    {
        return FuelBurnedDeltaV(parentShip.Mass(), requestedDeltaV, maxManeuverTime) <= fuel.AmountKg();
    }

    // Start is called before the first frame update
    void Start()
    {
        
    }

    // Update is called once per frame
    void Update()
    {
        
    }
}

public class ChemicalEngine : Engine { public ChemicalEngine(Fuel fuel_in, double mass, Ship parent) : base(fuel_in, 1e60, mass, parent) { } } // maximum engine power is not needed for chemical engines