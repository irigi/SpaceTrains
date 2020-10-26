using System.Collections;
using System.Collections.Generic;
using UnityEngine;

public class FlightState
{
    /// Contains information about the flight state of the ship
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
    Ship parentShip;

    public FlightState(Planet parent, StateEnum state, Ship parentShipIn)
    {
        parentBody = parent;
        flightState = state;
        parentShip = parentShipIn;
    }

    public delegate void OnFlightStateChange(FlightState previous, FlightState next);
    public delegate void SetupOrbitDelegate(InterpolatedOrbit orb);

    public bool SetNextFlightState(FlightState next, OnFlightStateChange onFlightStateChange)
    {
        // inconsistent parent body
        if (parentBody != next.parentBody && next.flightState != StateEnum.InterplanetaryTransfer && flightState != StateEnum.InterplanetaryTransfer) { return false; }
        if (next.parentBody == null) { Debug.Log("Attempt to start flight with null parent body"); return false; }

        // from surface only to LO
        if (flightState == StateEnum.OnSurface && next.flightState != StateEnum.LO) { return false; }

        // orbit if and only if next phase is interplanetary transfer
        //if (next.flightState == StateEnum.InterplanetaryTransfer && newOrbit == null) { return false; }
        //if (next.flightState != StateEnum.InterplanetaryTransfer && newOrbit != null) { return false; }

        if (next.flightState == StateEnum.InterplanetaryTransfer)
        {
            if (next.parentBody == null) { Debug.Log("Attempt to start interplanetary flight without specifiying the destination"); return false; }

            // TODO: Move the condition to Autopilot
            if (Orbit.TimeUntilHohmann(parentBody, next.parentBody) < 0.002)
            {
                Debug.Log("Launch");
                Vector3 pos0 = parentBody.transform.position;
                Vector3 v0 = new Vector3(-pos0.normalized[1], pos0.normalized[0]) * (float)Orbit.HohmannVelocity(parentBody, next.parentBody);
                InterpolatedOrbit newOrbit = new InterpolatedOrbit(pos0, v0, Time.time);
                parentShip.SetupOrbit(newOrbit);
            }
            else { return false; }
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

public class Autopilot
{
    /// Issue instructions about where to go next
    public FlightState target;
    Ship parent;

    public Autopilot(Ship parent_in)
    {
        parent = parent_in;
    }

    public void Update()
    {
        if (target != null)
        {
            if (parent.flightState.parentBody != target.parentBody)
            {
                if (parent.flightState.flightState == FlightState.StateEnum.OnSurface && parent.flightState.parentBody != null)
                {
                    // launch to low orbit
                    if (parent.flightState.SetNextFlightState(new FlightState(parent.flightState.parentBody, FlightState.StateEnum.LO, parent), null))
                    { Debug.Log($"{parent.name} launched to LO of {parent.flightState.parentBody.name}"); }
                }

                if (parent.flightState.flightState != FlightState.StateEnum.OnSurface && parent.flightState.parentBody != null)
                {
                    // launch to interplanetary transfer orbit                
                    if (parent.flightState.SetNextFlightState(new FlightState(target.parentBody, FlightState.StateEnum.InterplanetaryTransfer, parent), null))
                    { Debug.Log($"{parent.name} launched to interplanetary transfer orbit"); }
                }

            }
            else
            {
                if (parent.flightState.flightState == FlightState.StateEnum.InterplanetaryTransfer)
                {
                    // arrival at destination
                    if ((parent.gameObject.transform.localPosition - target.parentBody.gameObject.transform.localPosition).sqrMagnitude < 0.0001)
                    {
                        if (parent.flightState.SetNextFlightState(new FlightState(target.parentBody, FlightState.StateEnum.LO, parent), null))
                        { Debug.Log($"{parent.name} arrived from interplanetary orbit to {target.parentBody.name}"); }
                    }
                }
                //Debug.Log("TBD");
            }
        } else { Debug.Log("Autopilot target null"); }
    }



}

public class DeltaVCalculator
{

}



public class Ship : MonoBehaviour
{
    /// The class represents a ship. It is attached to the ship game objects and contains reference to the ship trajectory. In the future, it should aggregate classes
    /// for cargo, flight state, autopilot control, etc.
    
    public LineRenderer orbitRenderer = null;
    //public Planet destination = null;
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
        ship.autopilot = new Autopilot(ship);
        ship.autopilot.target = new FlightState(dest.GetComponent<Planet>(), FlightState.StateEnum.OnSurface, ship);
        ship.flightState = new FlightState(source.GetComponent<Planet>(), FlightState.StateEnum.OnSurface, ship);
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
        Destroy(orbitRenderer);
        orbitRenderer = null;
        orbit = null;
    }
}
