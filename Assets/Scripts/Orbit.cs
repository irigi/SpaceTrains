using System.Collections;
using System.Collections.Generic;
using UnityEngine;
using System;

public class InterpolatedOrbit
{
    List<Vector3> pts = new List<Vector3>();
    List<float> time = new List<float>();
    double period = 0;  // positive if closed

    public List<Vector3> GetPts()
    {
        return pts;
    }

    public InterpolatedOrbit(Vector3 position, Vector3 velocity, float t0)
    {
        double x = position[0], y = position[1], z = position[2];
        double vx = velocity[0], vy = velocity[1], vz = velocity[2];
        double t = t0; const double dt = 1 / 3650.0;
        double ax, ay, rg, phi0 = Math.Atan2(y, x), phiTotal=0, lastPhiTotal=0;

        z = 0; vz = 0;      // for now, we stay in-plane
        
        // stupid Eulerian integrator
        for (int i = 0; i < 100000; ++i)
        {
            if (i % 10 == 0)
            {
                pts.Add(new Vector3((float)x, (float)y, (float)z));
                time.Add((float)t);
            }

            x = vx * dt + x;
            y = vy * dt + y;
            rg = Orbit.gravityParameter / Math.Pow(x * x + y * y, 3.0 / 2);
            ax = -x * rg;
            ay = -y * rg;
            vx = vx + ax * dt;
            vy = vy + ay * dt;
            t = t + dt;

            lastPhiTotal = phiTotal;
            phiTotal = (2 * Math.PI + Math.Atan2(y, x) - phi0) % (2 * Math.PI);
            if (phiTotal < lastPhiTotal)
            {
                period = t - t0;
                break;                
            }
        }
    }

    public Vector3 CurrentPosition(float t)
    {
        Vector3 pos = pts[0];
        int len = pts.Count;
        while(t > time[time.Count-1])
        {
            t -= (float)period;
        }
        for (int i = 0; i < len-1; ++i)
        {
            if (time[i] <= t && time[i+1] > t)
            {
                pos = pts[i] + (pts[i + 1] - pts[i]) * (t - time[i]) / (time[i + 1] - time[i]);
            }
        }

        return pos;
    }

    public Vector3 CurrentVelocity(float t)
    {
        return (CurrentPosition(t) - CurrentPosition(t - 0.01f)) / 0.01f;
    }
}

public class Orbit : MonoBehaviour
{
    Planet planet;
    public const double gravityParameter = 39.4876393;
    public const double AU_PER_YEAR_TO_MPS = 4740.57172;

    public Vector3 CurrentPosition(float t)
    {
        float R = planet.GetScaledDist();
        double phi = AnglePhi(t);
        return new Vector3((float)(R * Math.Cos(phi)), (float)(R * Math.Sin(phi)), 0);
    }

    public Vector3 CurrentVelocity(float t)
    {
        return (CurrentPosition(t) - CurrentPosition(t - 0.01f)) / 0.01f;
    }

    // Start is called before the first frame update
    void Start()
    {
        Time.timeScale = 0.1f;
        Time.fixedDeltaTime = Time.fixedDeltaTime / 100;
        planet = gameObject.GetComponent<Planet>();
        //Debug.Log($"{m_planet.name} has mass {m_planet.mass} and its parent is {m_star.name}");
    }

    // Update is called once per frame
    void Update()
    {
        if (gameObject.name != "Sun")
        {
            float q = 1;
            gameObject.transform.localPosition = CurrentPosition(Time.timeSinceLevelLoad) / q;
        }
    }

    public double PeriodT()
    {
        return 2 * Math.PI * Math.Sqrt(planet.dist * planet.dist * planet.dist / gravityParameter / GameEvents.current.sun.mass);
    }

    static public double OrbitalVelocity(Planet planet)
    {
        return Math.Sqrt(gravityParameter / planet.dist);
    }

    public double AnglePhi(float t)
    {
        return (2 * Math.PI * t / PeriodT()) % (2 * Math.PI);
    }

    static public double HohmannTransferTime(Planet a, Planet b)
    {
        return Math.PI * Math.Sqrt(Math.Pow(a.dist + b.dist, 3) / 8 / gravityParameter);
    }

    static public double TimeUntilHohmann(Planet a, Planet b)
    {
        float t = Time.timeSinceLevelLoad;
        double transferT = HohmannTransferTime(a, b);
        double phiA = a.orbit.AnglePhi(t);
        double phiB = b.orbit.AnglePhi(t);
        double missingAngleB = (6 * Math.PI - phiA + b.orbit.AnglePhi(t) - Math.PI * (1 - 2 * HohmannTransferTime(a, b) / b.orbit.PeriodT())) % (2 * Math.PI);

        double syncT = Math.Abs(1 / (1 / b.orbit.PeriodT() - 1 / a.orbit.PeriodT()));
        return missingAngleB / (2*Math.PI) * syncT;
    }

    static public double HohmannVelocityDeparture(Planet a, Planet b)
    {
        // unit: AU / year
        return Math.Sqrt(2 * b.dist * gravityParameter / a.dist / (a.dist + b.dist));
    }

    static public double HohmannVelocityArrival(Planet a, Planet b)
    {
        // unit: AU / year
        return Math.Sqrt(2 * a.dist * gravityParameter / b.dist / (a.dist + b.dist));
    }

    static public double HohmannDeltaVelocityDeparture(Planet a, Planet b)
    {
        // unit: AU / year
        return HohmannVelocityDeparture(a, b) - OrbitalVelocity(a);
    }

    static public double HohmannDeltaVelocityArrival(Planet a, Planet b)
    {
        // unit: AU / year
        return OrbitalVelocity(b) - HohmannVelocityArrival(a, b);
    }

    static public double VelToMps(double vel) { return vel * AU_PER_YEAR_TO_MPS; }
}
