using System.Collections;
using System.Collections.Generic;
using UnityEngine;

public class PlanetController : MonoBehaviour
{
    //public Material Dark = Resources.Find;
    //public Material DarkRed;

    void Awake()
    {
        
    }

    // Start is called before the first frame update
    void Start()
    {
        GameEvents.current.onPlanetClick += OnClick;
        if (!GameEvents.current.allPlanets.Contains(gameObject))
        {            
            GameEvents.current.allPlanets.Add(gameObject);
        }

        if (gameObject.name == "Sun") { GameEvents.current.sun = gameObject.GetComponent<Planet>(); }
        if (gameObject.name == "Earth") { GameEvents.current.earth = gameObject; }
        if (gameObject.name == "Mars") { GameEvents.current.mars = gameObject; }

        SetupPlanetOrbit();
    }

    private void SetupPlanetOrbit()
    {
        LineRenderer template = (LineRenderer)Resources.Load("OrbitPrefab", typeof(LineRenderer));

        gameObject.GetComponent<Planet>().orbitRenderer = Instantiate(template);
        LineRenderer newRen = gameObject.GetComponent<Planet>().orbitRenderer;
        newRen.name = $"{gameObject.name} Orbit";
        newRen.transform.parent = gameObject.transform.parent;
        List<Vector3> positions = new List<Vector3>();
        float rad = (float)gameObject.GetComponent<Planet>().dist;
        int nn = 100;
        for (int i = 0; i < nn; i++)
        {
            positions.Add(new Vector3(rad * Mathf.Cos(2 * Mathf.PI * i / nn), rad * Mathf.Sin(2 * Mathf.PI * i / nn), 0));
        }
        newRen.positionCount = nn;
        newRen.SetPositions(positions.ToArray());
    }

    private void OnClick()
    {
        
    }

    public void OnMouseDown()
    {
        Debug.Log($"Planet {this.name} selected, distance {this.transform.position.magnitude}");

        // updating the selected list
        if(GameEvents.current.selected.Contains(gameObject))
        {            
            GameEvents.current.selected.Clear();
        } else
        {
            GameEvents.current.selected.Add(gameObject);
        }

        if (GameEvents.current.selected.Contains(gameObject))
        {
            gameObject.GetComponent<MeshRenderer>().material.color = Color.red;
        }
        else
        {
            gameObject.GetComponent<MeshRenderer>().material.color = new Color(70 / 256.0f, 70 / 256.0f, 70 / 256.0f);
        }
        GameEvents.current.PlanetClick();
    }
}
