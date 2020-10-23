using System.Collections;
using System.Collections.Generic;
using UnityEngine;
using System;

public class Orbit : MonoBehaviour
{
    Planet m_planet;
    public const float gravityParameter = 39.4876393f;

    // Start is called before the first frame update
    void Start()
    {
        Time.timeScale = 0.01f;
        Time.fixedDeltaTime = Time.fixedDeltaTime / 100;
        m_planet = gameObject.GetComponent<Planet>();
        //Debug.Log($"{m_planet.name} has mass {m_planet.mass} and its parent is {m_star.name}");
    }

    // Update is called once per frame
    void Update()
    {
        if (gameObject.name != "Sun")
        {
            float R = m_planet.GetScaledDist();
            double T = 2 * Math.PI * Math.Sqrt(m_planet.dist * m_planet.dist * m_planet.dist / gravityParameter / GameEvents.current.sun.mass);
            double phi = 2 * Mathf.PI * Time.timeSinceLevelLoad / T;
            double q = GameEvents.current.sun.radius_au;
            q = 1;
            gameObject.transform.localPosition = new Vector3((float)(1 /q * R * Math.Cos(phi)), (float)(1 /q * R * Math.Sin(phi)), 0);
        }
    }


}
