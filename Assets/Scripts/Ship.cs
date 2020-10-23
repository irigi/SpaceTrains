using System.Collections;
using System.Collections.Generic;
using UnityEngine;

public class Ship : MonoBehaviour
{
    public LineRenderer orbitRenderer = null;
    Planet destination = null;
    InterpolatedOrbit orbit = null;
    GameObject parentGO = null;

    // Start is called before the first frame update
    void Start()
    {
    }

    // Update is called once per frame
    void Update()
    {
        // update positions according to trajectory
        gameObject.transform.localPosition = orbit.CurrentPosition(Time.time);

        // destroy if target is reached     --- this should create event, otherwise is too fragile
        if ((gameObject.transform.localPosition - destination.transform.localPosition).sqrMagnitude < 0.0001)
        { Destroy(orbitRenderer.gameObject);  Destroy(parentGO);  }
    }

    static public GameObject SetupInstance(GameObject source, GameObject dest, InterpolatedOrbit orbit)
    {
        GameObject template = (GameObject)Resources.Load("ShipPrefab", typeof(GameObject));

        GameObject newRen = Instantiate(template);        
        newRen.name = $"Ship #{GameEvents.current.GetNextShipNr()}";

        newRen.transform.parent = source.transform.parent;
        newRen.transform.SetPositionAndRotation(source.transform.position, new Quaternion());           
        Ship ship = newRen.GetComponent<Ship>();
        ship.destination = dest.GetComponent<Planet>();
        ship.orbit = orbit;
        ship.parentGO = newRen;

        // orbit setup
        ship.SetupOrbit();

        return newRen;
    }

    void SetupOrbit()
    {
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
}
