SpaceTrains @VERSION@
=====================

A simulation of space logistics in the solar system: ships, stations and their economy.

Running
-------
Windows: double-click SpaceTrains.exe. Windows may warn that the program is from an unknown
publisher (it is not signed): choose "More info", then "Run anyway".
Linux:   run ./SpaceTrains.x86_64 (needs a Vulkan driver and glibc 2.39 or newer, e.g. Ubuntu 24.04).

Keep the folder together: the game starts the simulation from bin/ and reads data/ and atlas/.
The folder itself is never written to.

Keys: H shows the help. F5 saves, F9 loads.

Your files
----------
Saves, caches and logs are kept per user:
  Windows: %APPDATA%\Godot\app_userdata\SpaceTrains
  Linux:   ~/.local/share/godot/app_userdata/SpaceTrains
  saves/quicksave.json   the quicksave (F5 / F9)
  logs/godot.log         the game's log
  logs/bridge.log        the simulation's log
If something goes wrong, the two logs are what to send.

The first start takes a little longer: the simulation plans the opening fleet and keeps the
result for later starts.
