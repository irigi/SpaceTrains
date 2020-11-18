using System.Collections;
using System.Collections.Generic;
using UnityEngine;

public class FlightState
{
    /// Contains information about the flight state of the ship
    /// TODO: Introduce delays between orbits, surface, etc.

    public enum StateEnum
    {
        OnSurface,                      
        LO,                             // low orbit
        GO,                             // synchronous orbit
        FO,                             // far orbit
        InterplanetaryTransfer          // orbiting Sun, trajectory explicitly calculated
    }

    public StateEnum flightState { get; private set; }
    public Planet parentBody { get; private set; }
    public Ship parentShip { get; private set; }

    public FlightState(FlightState a)
    {
        parentBody = a.parentBody;
        flightState = a.flightState;
        parentShip = a.parentShip;
    }

    public FlightState(Planet parent, StateEnum state, Ship parentShipIn)
    {
        parentBody = parent;
        flightState = state;
        parentShip = parentShipIn;
    }

    public bool SetNextFlightState(FlightState next, InterpolatedOrbit newOrbit, double insertionDeltaV)
    {
        // inconsistent parent body
        if (parentBody != next.parentBody && next.flightState != StateEnum.InterplanetaryTransfer && flightState != StateEnum.InterplanetaryTransfer) { return false; }
        if (next.parentBody == null) { Debug.Log("Attempt to start flight with null parent body"); return false; }

        // from surface only to LO
        if (flightState == StateEnum.OnSurface && next.flightState != StateEnum.LO) { return false; }

        if (next.flightState == StateEnum.InterplanetaryTransfer)
        {
            if (next.parentBody == null) { Debug.Log("Attempt to start interplanetary flight without specifiying the destination"); return false; }
            if (newOrbit == null) { Debug.Log("Orbit required and not provided"); return false; }

            parentShip.SetupOrbit(newOrbit);            
        }

        if (flightState == StateEnum.InterplanetaryTransfer && next.flightState != StateEnum.InterplanetaryTransfer)
        {
            parentShip.DestroyOrbit();
        }

        parentShip.engine.BurnFuelByDeltaV(insertionDeltaV, 3600 / 2);
        flightState = next.flightState;
        parentBody = next.parentBody;

        return true;
    }
}

public class Ship : MonoBehaviour
{
    /// The class represents a ship. It is attached to the ship game objects and contains reference to the ship trajectory. In the future, it should aggregate classes
    /// for cargo, flight state, autopilot control, etc.
    
    public LineRenderer orbitRenderer = null;
    public InterpolatedOrbit orbit = null;
    public Engine engine = null;
    public List<Resource> carriedResources = new List<Resource>();
    public List<FuelTank> fuelTanks = new List<FuelTank>();

    Autopilot autopilot;
    public FlightState flightState { get; private set; }

    // Start is called before the first frame update
    void Start()
    {
    }

    // Update is called once per frame
    void Update()
    {
        // update positions according to trajectory
        if (orbit != null) { gameObject.transform.localPosition = orbit.CurrentPosition(Time.timeSinceLevelLoad); }
        else
        {
            if (flightState.parentBody != null) { gameObject.transform.localPosition = flightState.parentBody.transform.localPosition; }
        }

        autopilot.Update();
    }

    void OnDestroy()
    {
        GameEvents.current.allShips.Remove(gameObject);
        if (orbitRenderer != null) { Destroy(orbitRenderer.gameObject); }
    }

    public void Refuel()
    {
        Debug.Log($"Refueling");
        foreach (FuelTank tank in fuelTanks)
        {
            tank.resource.amountKg = tank.capacity;
        }
    }

    public double Mass()
    {
        double amount = 0;
        amount += engine.Mass();
        foreach(FuelTank fuelTank in fuelTanks) { amount += fuelTank.Mass(); }
        return amount;
    }

    static public GameObject SetupInstance(GameObject source, GameObject dest)
    {
        GameObject template = (GameObject)Resources.Load("ShipPrefab", typeof(GameObject));

        GameObject newRen = Instantiate(template);
        newRen.name = $"Ship #{GameEvents.current.GetNextShipNr()}";

        newRen.transform.parent = source.transform.parent;
        newRen.transform.SetPositionAndRotation(source.transform.position, new Quaternion());

        Ship ship = newRen.GetComponent<Ship>();
        ship.flightState = new FlightState(source.GetComponent<Planet>(), FlightState.StateEnum.OnSurface, ship);

        FlightState autopilotTarget = new FlightState(dest.GetComponent<Planet>(), FlightState.StateEnum.OnSurface, ship);
        ship.autopilot = new HohmannTransferPendlerAutopilot(ship, ship.flightState, autopilotTarget);
        ship.autopilot.target = autopilotTarget;

        // fuel and engine initialization
        RP1Resource rp1 = new RP1Resource();
        LOXResource lox = new LOXResource();        
        Fuel fuel = new RP1Fuel(rp1, lox);
        double fuelMass = 100000;
        FuelTank rp1Tank = new FuelTank(fuelMass, rp1, fuelMass*0.07);
        FuelTank loxTank = new FuelTank(fuelMass * rp1.oxidizer_ratio, lox, fuelMass * rp1.oxidizer_ratio*0.07);
        ship.engine = new ChemicalEngine(fuel, fuelMass*0.07, ship);
        ship.fuelTanks.Add(rp1Tank);
        ship.fuelTanks.Add(loxTank);
        ship.Refuel();
        
        ship.orbit = null;
        ship.orbitRenderer = null;

        GameEvents.current.allShips.Add(newRen);
        GameEvents.current.UpdateScales();

        return newRen;  
    }

    public void SetupOrbit(InterpolatedOrbit orb)
    {
        orbit = orb;

        LineRenderer template = (LineRenderer)Resources.Load("OrbitPrefab", typeof(LineRenderer));

        orbitRenderer = Instantiate(template);

        orbitRenderer.name = $"{gameObject.name} Orbit";
        orbitRenderer.transform.parent = gameObject.transform.parent;
        List<Vector3> positions = orbit.GetPts();
        orbitRenderer.positionCount = positions.Count;
        orbitRenderer.SetPositions(positions.ToArray());
        Color c = new Color(0 / 256.0f, 0 / 256.0f, 256 / 256.0f);
        orbitRenderer.startColor = c;
        orbitRenderer.endColor = c;
    }

    public void ChangeColor(Color c)
    {
        orbitRenderer.startColor = c;
        orbitRenderer.endColor = c;
        gameObject.GetComponent<MeshRenderer>().material.color = c;
    }

    public void DestroyOrbit()
    {
        Destroy(orbitRenderer.gameObject);
        orbitRenderer = null;
        orbit = null;
    }
}
