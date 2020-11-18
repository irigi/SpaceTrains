using System.Collections;
using System.Collections.Generic;
using UnityEngine;
using System;


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
    protected Ship parent;
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
                    double deltaV = StartToLeoDeltaV(parent.flightState.parentBody);
                    if (parent.flightState.SetNextFlightState(new FlightState(parent.flightState.parentBody, FlightState.StateEnum.LO, parent), null, deltaV))
                    { Debug.Log($"{parent.name} launched to LO of {parent.flightState.parentBody.name}"); }
                }

                if (parent.flightState.flightState != FlightState.StateEnum.OnSurface && parent.flightState.parentBody != null && InLaunchWindow(parent.flightState.parentBody, target.parentBody))
                {
                    InterpolatedOrbit proposedOrbit = TransferOrbit(parent.flightState.parentBody, target.parentBody);

                    double deltaV = StartInsertionBurnDeltaV(parent.flightState.parentBody, target.parentBody);
                    if (parent.engine.CanProvideDeltaV(deltaV, 3600 / 2))
                    {
                        // launch to interplanetary transfer orbit                
                        if (parent.flightState.SetNextFlightState(new FlightState(target.parentBody, FlightState.StateEnum.InterplanetaryTransfer, parent), proposedOrbit, deltaV))
                        { Debug.Log($"{parent.name} launched to interplanetary transfer orbit towards {target.parentBody.name}, fuel {parent.fuelTanks[0].resource.amountKg:F0}, {parent.fuelTanks[1].resource.amountKg:F0}"); }
                    } else
                    {
                        Debug.Log($"{parent.name} does not have fuel for insertion burn to {target.parentBody.name}");
                    }
                }

            }
            else
            {
                if (parent.flightState.flightState == FlightState.StateEnum.InterplanetaryTransfer)
                {
                    // arrival at destination
                    if ((parent.gameObject.transform.localPosition - target.parentBody.gameObject.transform.localPosition).sqrMagnitude < 0.00025)
                    {
                        double deltaV = EndInsertionBurnDeltaV(parent.flightState.parentBody, target.parentBody);
                        if (parent.engine.CanProvideDeltaV(deltaV, 3600 / 2))
                        {
                            if (parent.flightState.SetNextFlightState(new FlightState(target.parentBody, target.flightState, parent), null, deltaV))
                            {
                                Debug.Log($"{parent.name} arrived from interplanetary orbit to {target.flightState} of {target.parentBody.name}, fuel {parent.fuelTanks[0].resource.amountKg:F0}, {parent.fuelTanks[1].resource.amountKg:F0}");
                                parent.Refuel();
                            }
                            else
                            { Debug.Log($"Setting flight state failed: {parent.name}, {target.flightState} of {target.parentBody.name}"); }
                        }
                        else
                        {
                            Debug.Log($"Ship {parent.name} run out of fuel!");
                        }
                    }
                }                
            }
        } else { Debug.Log("Autopilot target null"); }
    }

    abstract protected bool InLaunchWindow(Planet from, Planet to);
    abstract protected InterpolatedOrbit TransferOrbit(Planet from, Planet to);
    abstract protected double StartInsertionBurnDeltaV(Planet from, Planet to);
    abstract protected double EndInsertionBurnDeltaV(Planet from, Planet to);
    virtual protected double StartToLeoDeltaV(Planet from)
    {
        //return Orbit.OrbitalVelocity(from);
        return 1;
    }

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
        Vector3 v0 = new Vector3(-pos0.normalized[1], pos0.normalized[0]) * (float)Orbit.HohmannVelocityDeparture(from, to);
        InterpolatedOrbit newOrbit = new InterpolatedOrbit(pos0, v0, Time.timeSinceLevelLoad);
        return newOrbit;
    }

    protected override double StartInsertionBurnDeltaV(Planet from, Planet to)
    {
        return Math.Abs(Orbit.VelToMps(Orbit.HohmannDeltaVelocityDeparture(from, to)));
    }

    protected override double EndInsertionBurnDeltaV(Planet from, Planet to)
    {
        return Orbit.VelToMps((parent.orbit.CurrentVelocity(Time.time) - parent.flightState.parentBody.orbit.CurrentVelocity(Time.time)).magnitude);
    }
}

public class HohmannTransferPendlerAutopilot : HohmannTransferAutopilot
{
    public HohmannTransferPendlerAutopilot(Ship parent_in, FlightState start, FlightState end) : base(parent_in, new PendlerTargetSelector(start, end)) { }
}

