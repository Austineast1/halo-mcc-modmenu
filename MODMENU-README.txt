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
- Pick a tab on the left: Spartan, Become, Arsenal, World, Battle, Mission,
  HUD and Camera, Skulls, Halo CE Classic, Custom.
- Use the search bar to find any mod by name.
- The game chips on each row (CE H2 H3 ODST REACH H4) show which games
  that mod works in. The one you're playing is highlighted.
- The ENGINE BUS panel shows every command sent to the game and what the
  game replied. Red lines mean that command didn't work in this game.
- Mods marked (r) turn themselves back on after you die, revert a
  checkpoint, or load the next level.


BECOME A HUNTER
---------------
Halo CE:  Become tab > turn on "Body Swap", then walk into a Hunter.
          You take over its body. "Summon Every Character" spawns a crowd
          to pick from.
Other games: "Hunter Mode" makes you Hunter-sized and Hunter-tough and puts
          the Covenant on your side. It is not a full body swap.


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
