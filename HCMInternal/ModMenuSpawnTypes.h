#pragma once
#include <cstdint>
#include <string>

// Shared between the spawner (engine side) and the menu (UI side). Kept free of HCM headers on purpose.
enum class SpawnKind : int { Weapon, Vehicle, Character, Equipment, Prop, Count };

struct ModMenuSpawnEntry
{
	uint32_t tag;			// tag datum
	SpawnKind kind;
	std::string name;		// "Assault Rifle"
	std::string path;		// "objects\weapons\rifle\assault_rifle\assault_rifle"
};
