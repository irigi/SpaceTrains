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

    public delegate void OnFlightStateChange(FlightState previous, FlightState next);

    public bool SetNextFlightState(FlightState next, OnFlightStateChange onFlightStateChange, InterpolatedOrbit newOrbit)
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

        onFlightStateChange?.Invoke(this, next);
        flightState = next.flightState;
        parentBody = next.parentBody;

        return true;
    }
}

public abstract class TargetSelector
{
    abstract public FlightState OnTargetReached(FlightState flightStateNow);
}

public class PendlerTargetSelector : TargetSelector
{
    FlightState a, b;

    public PendlerTargetSelector(FlightState a_in, FlightState b_in)
    {
        a = new FlightState(a_in);
        b = new FlightState(b_in);
    }

    public override FlightState OnTargetReached(FlightState flightStateNow)
    {
        if (flightStateNow.flightState == a.flightState && flightStateNow.parentBody == a.parentBody) { return new FlightState(b); }
        if (flightStateNow.flightState == b.flightState && flightStateNow.parentBody == b.parentBody) { return new FlightState(a); }
        Debug.Log($"Pendler lost in state {flightStateNow.parentBody}-{flightStateNow.flightState}"); return null;
    }
}

public abstract class Autopilot
{
    /// Issue instructions about where to go next
    /// TODO: launch from surface only before the transfer window

    public FlightState target;
    Ship parent;
    TargetSelector targetSelector;

    protected Autopilot(Ship parent_in, TargetSelector selector)
    {
        parent = parent_in;
        targetSelector = selector;
    }

    public void Update()
    {
        if (parent.flightState.parentBody == target.parentBody && parent.flightState.flightState == target.flightState)
        {
            OnTargetReached();
        }

        if (target != null)
        {
            if (parent.flightState.parentBody != target.parentBody)
            {
                if (parent.flightState.flightState == FlightState.StateEnum.OnSurface && parent.flightState.parentBody != null)
                {
                    // launch to low orbit
                    if (parent.flightState.SetNextFlightState(new FlightState(parent.flightState.parentBody, FlightState.StateEnum.LO, parent), null, null))
                    { Debug.Log($"{parent.name} launched to LO of {parent.flightState.parentBody.name}"); }
                }

                if (parent.flightState.flightState != FlightState.StateEnum.OnSurface && parent.flightState.parentBody != null && InLaunchWindow(parent.flightState.parentBody, target.parentBody))
                {
                    // launch to interplanetary transfer orbit                

                    if (parent.flightState.SetNextFlightState(new FlightState(target.parentBody, FlightState.StateEnum.InterplanetaryTransfer, parent), null,
                        TransferOrbit(parent.flightState.parentBody, target.parentBody)))
                    { Debug.Log($"{parent.name} launched to interplanetary transfer orbit towards {target.parentBody.name}"); }
                }

            }
            else
            {
                if (parent.flightState.flightState == FlightState.StateEnum.InterplanetaryTransfer)
                {
                    // arrival at destination
                    if ((parent.gameObject.transform.localPosition - target.parentBody.gameObject.transform.localPosition).sqrMagnitude < 0.00025)
                    {
                        if (parent.flightState.SetNextFlightState(new FlightState(target.parentBody, target.flightState, parent), null, null))
                        { Debug.Log($"{parent.name} arrived from interplanetary orbit to {target.flightState} of {target.parentBody.name}"); }
                        else
                        { Debug.Log($"Setting flight state failed: {parent.name}, {target.flightState} of {target.parentBody.name}"); }
                    }
                }                
            }
        } else { Debug.Log("Autopilot target null"); }
    }

    abstract protected bool InLaunchWindow(Planet from, Planet to);
    abstract protected InterpolatedOrbit TransferOrbit(Planet from, Planet to);

    void OnTargetReached()
    {
        FlightState old = target;
        target = targetSelector.OnTargetReached(target);
        Debug.Log($"Target switched from {old.parentBody}-{old.flightState} to {target.parentBody}-{target.flightState}");
    }
}

public class HohmannTransferAutopilot : Autopilot
{
    /// Provides Hohmann transfer launch windows and trajectories

    public HohmannTransferAutopilot(Ship parent_in, TargetSelector targetSelector) : base(parent_in, targetSelector) { }

    protected override bool InLaunchWindow(Planet from, Planet to)
    {
        //Debug.Log($"Hohmann {Orbit.TimeUntilHohmann(from, to)}");
        return Orbit.TimeUntilHohmann(from, to) < 0.002;
    }

    protected override InterpolatedOrbit TransferOrbit(Planet from, Planet to)
    {
        Vector3 pos0 = from.transform.position;
        Vector3 v0 = new Vector3(-pos0.normalized[1], pos0.normalized[0]) * (float)Orbit.HohmannVelocity(from, to);
        InterpolatedOrbit newOrbit = new InterpolatedOrbit(pos0, v0, Time.time);
        return newOrbit;
    }
}

public class HohmannTransferPendlerAutopilot : HohmannTransferAutopilot
{ public HohmannTransferPendlerAutopilot(Ship parent_in, FlightState start, FlightState end) : base(parent_in, new PendlerTargetSelector(start, end)) { }
}



public class DeltaVCalculator
{

}



public class Ship : MonoBehaviour
{
    /// The class represents a ship. It is attached to the ship game objects and contains reference to the ship trajectory. In the future, it should aggregate classes
    /// for cargo, flight state, autopilot control, etc.
    
    public LineRenderer orbitRenderer = null;
    public InterpolatedOrbit orbit = null;

    Autopilot autopilot;
    public FlightState flightState;

    // Start is called before the first frame update
    void Start()
    {
    }

    // Update is called once per frame
    void Update()
    {
        // update positions according to trajectory
        if (orbit != null) { gameObject.transform.localPosition = orbit.CurrentPosition(Time.time); }
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

    public void DestroyOrbit()
    {
        Destroy(orbitRenderer.gameObject);
        orbitRenderer = null;
        orbit = null;
    }
}
