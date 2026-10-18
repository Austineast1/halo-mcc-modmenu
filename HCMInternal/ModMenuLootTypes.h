#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Plain data shared by the Loot system and the menu (no HCM headers, so the GUI preview can use it too).

enum class LootRarity { Common, Uncommon, Rare, Epic, Legendary, Count };

struct LootStat
{
	std::string label;	// "Fire rate"
	std::string value;	// "x1.6"
	bool good = true;	// drawn brighter when it's an upgrade
};

struct LootWeapon
{
	uint32_t tag = 0xFFFFFFFF;
	std::string baseName;		// "Assault Rifle"
	std::string path;			// objects\weapons\rifle\assault_rifle\assault_rifle
	LootRarity rarity = LootRarity::Common;
	std::string maker;			// "Iruiru Armory"
	std::string title;			// "Hellfire" (legendaries and some epics), else empty
	std::string element;		// "Plasma", "Explosive"... empty when it fires its own rounds
	std::vector<LootStat> stats;
	std::vector<std::string> perks;	// short lines: "Fires 3 rounds per shot"
};
