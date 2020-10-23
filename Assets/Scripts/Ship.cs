using System.Collections;
using System.Collections.Generic;
using UnityEngine;

public class Ship : MonoBehaviour
{
    public LineRenderer orbitRenderer = null;
    Planet destination = null;

    // Start is called before the first frame update
    void Start()
    {
    }

    // Update is called once per frame
    void Update()
    {
        
    }

    static public GameObject SetupInstance(GameObject source, GameObject dest)
    {
        GameObject template = (GameObject)Resources.Load("ShipPrefab", typeof(GameObject));

        GameObject newRen = Instantiate(template);        
        newRen.name = $"Ship #{GameEvents.current.GetNextShipNr()}";

        newRen.transform.SetPositionAndRotation(source.transform.position, new Quaternion());

        Ship ship = newRen.GetComponent<Ship>();
        ship.destination = dest.GetComponent<Planet>();

        return newRen;
    }
}
