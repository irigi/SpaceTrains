﻿using System;
using System.Collections;
using System.Collections.Generic;
using UnityEngine;

public class GameEvents : MonoBehaviour
{
    public static GameEvents current;
    public List<GameObject> allPlanets;
    public List<GameObject> allShips;
    public List<GameObject> selected;

    // registered in Start of PlanetController
    public Planet sun = null;
    public GameObject earth = null;
    public GameObject mars = null;

    private int shipNr = 0;

    private void Awake()
    {
        current = this;
        selected = new List<GameObject>();
        allPlanets = new List<GameObject>();
    }

    private void Update()
    {
        Planet earthp = earth.GetComponent<Planet>();
        Planet marsp = mars.GetComponent<Planet>();
        if (shipNr == 0) { Ship.SetupInstance(earth, mars); }
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
        float z = -Camera.main.transform.position[2];
        float q = 1 / 50.0f;
        float r = 1 / 250.0f;
        foreach (GameObject planet in allPlanets)
        {
            planet.transform.localScale = new Vector3(z * q, z * q, z * q);
            planet.GetComponent<Planet>().orbitRenderer.startWidth = r * z;
            planet.GetComponent<Planet>().orbitRenderer.endWidth = r * z;
        }

        foreach (GameObject ship in allShips)
        {
            ship.transform.localScale = new Vector3(z * q, z * q, z * q);
            LineRenderer orbRen = ship.GetComponent<Ship>().orbitRenderer;
            if (orbRen != null)
            {
                orbRen.startWidth = r * z;
                orbRen.endWidth = r * z;
            }
        }
    }

    public int GetNextShipNr()
    {
        shipNr = shipNr + 1;
        return shipNr;
    }
}
