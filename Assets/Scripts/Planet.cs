using System.Collections;
using System.Collections.Generic;
using UnityEngine;

public class Planet : MonoBehaviour
{
    public double mass = 0;
    public double dist = 0;
    public double radius_au = 0;
    public LineRenderer orbitRenderer = null;
    public Orbit orbit = null;

    // Start is called before the first frame update
    void Start()
    {
        orbit = gameObject.GetComponent<Orbit>();
    }

    // Update is called once per frame
    void Update()
    {
    }

    public float GetScaledDist()
    {
        // Distance corresponds to escape velocity
        //return (92.1048756f - Mathf.Sqrt(Orbit.gravityParameter / dist)) / 10;

        return (float)dist;
        //return Mathf.Sqrt(dist)*10;
    }
}
