#pragma once
#include "pch.h"
#include "GameState.h"
#include "DIContainer.h"

#include "ModMenuSpawnTypes.h"

struct ModMenuPose
{
	SimpleMath::Vector3 position;	// player body origin, world units
	float yaw;						// radians
};

// Spawns any weapon / vehicle / character / equipment / crate the current level has loaded, in front of the player.
//
// Halo 3, ODST and Reach (MCC 1.3528): calls the game's own object_placement_data_new and object_new on the game
// thread, inside the game tick, because the engine keeps its game state in thread-local storage. Both functions are
// signature-checked before every call; nothing is called if the bytes don't match (another MCC version).
class ModMenuSpawner
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	// throws HCMInitException if spawning isn't supported for this game / MCC version
	ModMenuSpawner(GameState game, IDIContainer& dicon, std::function<std::optional<ModMenuPose>()> poseProvider);
	~ModMenuSpawner();

	// Render thread. Built lazily once per level. Empty if the tag table can't be read.
	const std::vector<ModMenuSpawnEntry>& catalogue();

	// Render thread. Queues a spawn for the next game tick; returns a message for the footer.
	std::string spawn(const ModMenuSpawnEntry& entry, bool thenBecomeIt);

	struct Result { std::string message; uint32_t object; bool becomeIt; };
	// Render thread, every frame: reports a finished spawn once.
	std::optional<Result> update();

	void onLevelChanged();
};
