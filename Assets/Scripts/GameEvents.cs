using System;
using System.Collections;
using System.Collections.Generic;
using UnityEngine;

public class GameEvents : MonoBehaviour
{
    public static GameEvents current;
    public List<GameObject> allPlanets;
    public List<GameObject> allShips;
    public List<GameObject> selected;
    public Planet sun = null;

    private int shipNr = 0;

    private void Awake()
    {
        current = this;
        selected = new List<GameObject>();
        allPlanets = new List<GameObject>();
    }

    private void Update()
    {
        if (shipNr == 0 && Time.time > 0.3)
        {
            GameObject earth = null;
            GameObject mars = null;
            foreach (GameObject p in GameEvents.current.allPlanets)
            {
                if (p.name == "Earth") { earth = p; }
                if (p.name == "Mars") { earth = p; }
            }

            GameObject newShip = Ship.SetupInstance(earth, mars);
            allShips.Add(newShip);
        }
    }

    public event Action onPlanetClick;
    public void PlanetClick()
    {
        if (onPlanetClick != null)
        {
            onPlanetClick();
        }
    }

    public void UpdateScales()
    {
        // planets are dimensionless, they always appear as balls of constant angular size
        float z = Camera.main.transform.position[2];
        float q = 1 / 50.0f;
        float r = 1 / 250.0f;
        foreach (GameObject planet in allPlanets)
        {
            planet.transform.localScale = new Vector3(z * q, z * q, z * q);
            planet.GetComponent<Planet>().orbitRenderer.startWidth = r * z;
            planet.GetComponent<Planet>().orbitRenderer.endWidth = r * z;
        }

        foreach (GameObject planet in allShips)
        {
            planet.transform.localScale = new Vector3(z * q, z * q, z * q);
            //planet.GetComponent<Planet>().orbitRenderer.startWidth = r * z;
            //planet.GetComponent<Planet>().orbitRenderer.endWidth = r * z;
        }
    }

    public int GetNextShipNr()
    {
        shipNr = shipNr + 1;
        return shipNr;
    }
}
