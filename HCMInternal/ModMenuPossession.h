#pragma once
#include "pch.h"
#include "GameState.h"
#include "DIContainer.h"
#include "ModMenuSpawner.h" // ModMenuPose

class GameEngineFunctions;

// True possession: the player takes over another unit's body (camera, movement, aim, weapons, melee).
//
// How it works (Halo: Reach, MCC 1.3528 — found by live memory inspection, Oct 2026):
//  - the player record holds the unit it drives (+0x28; the game copies it to +0x34 itself)
//  - every unit holds the player driving it (+0x1CC) and its AI actor (+0x1BC)
//  - two per-local-user mapping tables in the game state (player struct +0x4A2C and +0x4AE0)
//    decide which unit the camera follows and which unit receives input
// The swap rewrites those on the game thread (inside the game tick), then the possessed unit's AI is
// made braindead with HaloScript so it stops steering. Every field is checked before anything is written;
// if the layout isn't what we expect (another MCC version, a different game) nothing is touched.
class ModMenuPossession
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	// throws HCMInitException if possession isn't supported for this game / MCC version
	ModMenuPossession(GameState game, IDIContainer& dicon, std::shared_ptr<GameEngineFunctions> engine);
	~ModMenuPossession();

	// Render thread. Each returns a short message for the menu's footer.
	std::string possessAimed();	// take over the creature closest to the crosshair
	std::string returnToBody();	// go back to the body you started in
	std::string possessBody(uint32_t unitDatum);	// take over a specific body (e.g. one we just spawned)

	// where the player is and which way they face (render thread)
	std::optional<ModMenuPose> playerPose();

	// Render thread, every frame: finishes queued swaps and reports results (returns a message once).
	std::optional<std::string> update();

	void onLevelChanged();		// forget the original body (new level, new objects)

	// a swap is queued or just finished: the player's unit is about to change on purpose (not a death)
	bool busy();
};
