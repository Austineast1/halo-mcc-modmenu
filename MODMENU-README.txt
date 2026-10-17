HCM MOD MENU
============

An in-game mod menu for Halo: The Master Chief Collection campaign, built on
HaloCheckpointManager (HCM) by Burnt-o. Works in all six games.


FIRST TIME SETUP
----------------
1. Unzip this folder anywhere (Desktop is fine).
2. In Steam, press Play on Halo MCC and choose
   "Play Halo MCC Anti-Cheat Disabled".
   Mods never work with anti-cheat on, and you can't use matchmaking this way.
3. Run HaloCheckpointManager.exe from this folder.
   If Windows asks for the .NET 7 Desktop Runtime, install it and run it again.
4. Start any campaign mission.


USING THE MENU
--------------
Insert ........ open or close the Mod Menu
`  (grave) .... open HCM's own main menu (rebind keys there)

Inside the Mod Menu:
- Pick a collection on the left: Spartan, Become, Arsenal, World, Battle,
  Mission, HUD and Camera, Director, Chaos, Skulls, Halo CE Classic, Custom.
  Only mods that work in the game you're playing are shown.
- Flip the gold switches, press ACTIVATE for one-off effects, or drag a
  slider and press APPLY. Anything switched on gets a gold edge.
- Use the search bar to find any mod by name.
- The line at the bottom left confirms what just happened.
- SETTINGS (bottom right): re-apply mods after death, pause while the menu
  is open, show the engine log (every command sent to MCC), and text size.
- Mods that are on turn themselves back on after you die, revert a
  checkpoint, or load the next level.


BECOME
------
Halo CE:  Become > turn on "Possess Anything", then walk into any unit:
          a Hunter, an Elite, a Marine, a Flood form. You take over its body.
          "Summon Every Character" spawns a crowd to pick from.
Reach and Halo 4: "Join the Covenant" (and "Join the Brutes" in Reach) puts
          you on their team for real. Covenant treat you as their own and the
          humans shoot back.
Every game: one-press transformations (Hunter, Elite, Grunt, Brute, Drone,
          Engineer, Guta, Flood, Sentinel, Monitor, Prophet, Knight, Titan and
          more) that change your size, toughness, abilities and allies.
          "Back to Spartan" undoes them.
Tip: HUD and Camera > "Hide First-Person Weapon" makes it feel even more real.


ADD YOUR OWN MODS
-----------------
ModMenuLibrary.xml in this folder holds every mod. Open it in Notepad,
copy an existing <Mod> block, change it, save, then press RELOAD LIBRARY in
the menu. No rebuild needed. The top of the file explains every option.


CO-OP
-----
This menu runs on your PC only. Changing the game on one PC can cause an
out-of-sync error in co-op. Stick to solo play for the wild stuff.


CREDITS
-------
HaloCheckpointManager (c) Maurice Dix / Burnt-o, MIT License.
Mod Menu added on top of it. See LICENSE.txt.
