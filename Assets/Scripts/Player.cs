using System.Collections;
using System.Collections.Generic;
using UnityEngine;

public class Player : MonoBehaviour
{
    Camera m_Camera;
    const float m_CamSpeed = 75.0f;
    float m_zmin = 0;
    List<GameObject> m_Planets = new List<GameObject>();
    float m_timeScaleBeforePause = 0;

    // Start is called before the first frame update
    void Start()
    {
        m_Camera = Camera.main;
        List<string> names = new List<string>();
        foreach (GameObject planet in GameEvents.current.allPlanets)
        {
            names.Add(planet.name);
        }
        foreach (string name in names)
        {
            GameObject obj = GameObject.Find(name);
            if (obj != null)
            {
                m_Planets.Add(obj);
            }
            else
            {
                Debug.Log(name + " not found");
            }
        }
    }

    // Update is called once per frame
    void Update()
    {
        Controls();
    }

    private void Controls()
    {
        float z0 = -m_Camera.transform.position[2];
        float shift = Mathf.Exp((z0 - m_zmin) / 100) - 1;
        if (shift > 5) shift = 5;

        if (Input.GetKey(KeyCode.E))
        {
            if (z0 < m_zmin) shift = 0;
            m_Camera.transform.Translate(new Vector3(0, 0, m_CamSpeed * Time.unscaledDeltaTime * shift));
            GameEvents.current.UpdateScales();
        }

        if (Input.GetKey(KeyCode.Q))
        {
            if (z0 > 800) shift = 0;
            m_Camera.transform.Translate(new Vector3(0, 0, -m_CamSpeed * Time.unscaledDeltaTime * shift));
            GameEvents.current.UpdateScales();
        }

        if (Input.GetKey(KeyCode.LeftArrow) || Input.GetKey(KeyCode.A))
        {
            m_Camera.transform.Translate(new Vector3(-m_CamSpeed * Time.unscaledDeltaTime * shift, 0, 0));
        }

        if (Input.GetKey(KeyCode.RightArrow) || Input.GetKey(KeyCode.D))
        {
            m_Camera.transform.Translate(new Vector3(m_CamSpeed * Time.unscaledDeltaTime * shift, 0, 0));
        }

        if (Input.GetKey(KeyCode.UpArrow) || Input.GetKey(KeyCode.W))
        {
            m_Camera.transform.Translate(new Vector3(0, m_CamSpeed * Time.unscaledDeltaTime * shift, 0));
        }

        if (Input.GetKey(KeyCode.DownArrow) || Input.GetKey(KeyCode.S))
        {
            m_Camera.transform.Translate(new Vector3(0, -m_CamSpeed * Time.unscaledDeltaTime * shift, 0));
        }

        if (Input.GetKeyDown(KeyCode.Space))
        {
            if (Time.timeScale > 0)
            {
                m_timeScaleBeforePause = Time.timeScale;
                Time.timeScale = 0;
            }
            else
            {
                Time.timeScale = m_timeScaleBeforePause;
            }
        }

            if (Input.GetKeyDown(KeyCode.R))
        {
            Vector3 pos = Camera.main.transform.position;
            Quaternion rot = Camera.main.transform.rotation;
            Camera.main.transform.SetPositionAndRotation(new Vector3(0, 0, pos[2]), rot);
        }
    }
}
