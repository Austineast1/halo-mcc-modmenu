# HCM Mod Menu
*An in-game mod menu for Halo: The Master Chief Collection campaign, built on HaloCheckpointManager.*

**Download:** [latest build](../../releases/tag/latest) (`HCM-ModMenu.zip`). Unzip it, launch MCC with *Anti-Cheat Disabled*, run `HaloCheckpointManager.exe`, press **Insert** in a mission.

What this fork adds:
* **Mod Menu window** (Insert): 160+ mods in 12 collections for Halo CE, 2, 3, ODST, Reach and 4, in a black-and-gold overlay. Each game only shows the mods that work in it, between 110 and 125 per game plus that game's skulls.
* **Become**: a real body swap in Halo CE (walk into a Hunter and you *are* the Hunter), true team changes in Reach and Halo 4 (join the Covenant or the Brutes), and over 20 one-press transformations elsewhere: Hunter, Elite, Zealot, Arbiter, Grunt, Jackal, Brute, Chieftain, Drone, Engineer, Guta, Flood forms, Sentinel, Monitor, Prophet, Promethean Knight, Watcher, ODST, Titan and more.
* **Spartan, Arsenal, World, Battle, Mission, HUD and Camera, Director and Chaos** collections: anti-gravity, boss armour, vehicle nitro, gravity of other worlds, ceasefires and mutinies, unlimited fireteams, photo mode, flashbangs and more.
* **Live skulls** showing the real on/off state of every skull in the current game.
* **Auto re-apply**: size, camo, gravity, allegiances and other lasting mods come back after deaths, reverts and level loads.
* **Editable library**: every mod lives in `ModMenuLibrary.xml` next to the exe. Add your own and press *Reload*, no rebuild.
* **Checked against the engines**: `tools/validate_modmenu_library.py` tests every command against each game's official HaloScript function list, and the Reach set was also compiled live inside the running game.
* Builds automatically on GitHub Actions (`.github/workflows/build.yml`).

Mods are HaloScript commands sent through MCC's own engine command interface, so they don't depend on per-patch memory offsets. HCM's own cheats (invulnerability, speedhack, AI freeze, free camera and more) are bridged into the same menu.

---

# HaloCheckpointManager
*A practice tool for [Halo: The Master Chief Collection](https://store.steampowered.com/app/976730/Halo_The_Master_Chief_Collection/)*

---**Download**---  
From the [Releases page.](https://github.com/Burnt-o/HaloCheckpointManager/releases)  

---**Features**---
* Back up (dump) to file the games checkpoints, so you can inject them again at any later time. 
* UI for managing your checkpoint collection (ordering, sorting, renaming etc).
* Single-player cheats to help you practice and route the games, such as
   * Force custom checkpoints & back them up
   * Invincibility
   * Speedhack (both slow-down and fast-forward)
   * Block the games natural checkpoints
   * Teleport and Launch the player
   * And more (specific games have different features)
 * Support for multiple downpatched versions of MCC - Season 7 (2448), Season 8 (2645), and of course current patch.

---**Building from source**---   
To build the source yourself, grab the latest version of [Visual Studio](https://visualstudio.microsoft.com/) and the following workloads/components:  
 * .NET desktop development (and .NET 7.0 runtime)  
 * Desktop development with C++  
 * Windows 10 SDK (or higher)
    
HCM uses vcpkg manifest files for C++ dependecies. With Visual Studio this should all be automatic. 
I haven't tried building outside of this VS environment so YMMV if you try otherwise. 

This project also makes heavy use of the amazing SafetyHook by cursey. You can think of it as like "Microsoft Detours but if it didn't suck". I've included the release I'm using in this repository so you shouldn't need to worry about it.

After successfully building all projects (besides HCMInternalTests, that doesn't matter), make sure to rebuild the entire solution to ensure all build events fire correctly.

Known build issue: Your build events may fail to copy over certain files to the correct directory due to macro issues: fix this by adjusting the build event macros for each project to make sure they're outputting to HCMExternal's binary output directory.