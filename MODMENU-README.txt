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
- Pick a collection on the left: Spartan, Become, Spawn, Arsenal, World,
  Battle, Mission, HUD and Camera, Director, Chaos, Skulls, Halo CE Classic,
  Custom.
  Only mods that work in the game you're playing are shown.
- Flip the gold switches, press ACTIVATE for one-off effects, or drag a
  slider and press APPLY. Anything switched on gets a gold edge.
- Use the search bar to find any mod by name.
- The line at the bottom left confirms what just happened.
- SETTINGS (bottom right): re-apply mods after death, pause while the menu
  is open, show the engine log (every command sent to MCC), and text size.
- Mods that are on turn themselves back on after you die, revert a
  checkpoint, respawn in co-op, or load the next level. A short note in the
  corner of the screen tells you when that happens.


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
          Your form stays on after you die until you pick another one or
          press "Back to Spartan".
Halo: Reach true body swap: Become > "Take Over What You're Aiming At".
          Aim at any creature (or stand right next to it) and press it. You
          ARE that creature now: its camera, its walk, its weapons, its melee.
          "Return to Your Body" puts you back. Dying in a borrowed body sends
          you back to your last checkpoint like any other death.
Tip: HUD and Camera > "Hide First-Person Weapon" makes it feel even more real.


SPAWN (Halo 3, ODST and Reach)
-------------------------------
The Spawn collection lists every weapon, vehicle, character, piece of
equipment and crate the current part of the level has loaded. Pick a tab,
type in the filter, press SPAWN and it appears in front of you. In Reach,
characters also have a BECOME button: it spawns the body and puts you
straight inside it.
Missions only keep some vehicles in memory at a time. Press LOAD MORE FROM
THIS MISSION to load every extra set the mission allows right now
(vehicles, weapons and characters it would normally bring in later); the
list grows a few seconds later. What exists at all depends on the mission
(and on the Workshop mod you're playing): a Scorpion can only be spawned on
a mission that has one.


THIRD PERSON
------------
HUD and Camera > "Third Person" puts the camera behind your shoulder in any
game. You keep full control (walk, aim, shoot, drive), and it follows any
body you take over. "Third Person Distance" pulls it in or out.


STEAM WORKSHOP MODS
-------------------
The menu also works in Workshop campaign mods (they need "Anti-Cheat
Disabled" too). Mods that change creatures, teams or scripts can make some
Become forms and team switches act differently.


ADD YOUR OWN MODS
-----------------
ModMenuLibrary.xml in this folder holds every mod. Open it in Notepad,
copy an existing <Mod> block, change it, save, then press RELOAD (bottom right of
the menu). No rebuild needed. The top of the file explains every option.


FOR DEVELOPERS: SCRIPTED TESTING
--------------------------------
Put a file called ModMenuDev.txt in this folder while you're in a mission.
Each line is run (about once a second the file is picked up and deleted) and
the results are written to ModMenuDev.log. Commands:
  mods                       list every mod id for the game you're playing
  mod <id> [on|off|value]    run a mod (sliders take a number, choices an index)
  selftest [all|<collection>|<id prefix>] [after <id>]   run every mod one at a time;
                             if the game crashes, the last TEST line in the log
                             names the mod that did it.  "selftest stop" ends it
  alloff                     turn every mod off
  hs <command>               send one HaloScript command
  spawn <name> / become <name> / catalogue / loadmore   spawning
  possess / return / pose    (Reach) body swap
  status                     game, tick and player unit
  wait <ms>                  pause between lines


CO-OP
-----
This menu runs on your PC only. Changing the game on one PC can cause an
out-of-sync error in co-op. Stick to solo play for the wild stuff.


CREDITS
-------
HaloCheckpointManager (c) Maurice Dix / Burnt-o, MIT License.
Mod Menu added on top of it. See LICENSE.txt.
