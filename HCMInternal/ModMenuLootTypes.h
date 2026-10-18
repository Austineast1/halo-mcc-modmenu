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

// Weapon mastery: kills with a gun (kept across missions and sessions, per gun type) level it up, and every level makes
// that gun a bit better, on top of whatever loot it rolled.
inline constexpr int kMasteryMaxLevel = 5;
inline constexpr int kMasteryKills[kMasteryMaxLevel + 1] = { 0, 10, 30, 75, 150, 300 };	// kills to reach each level
inline const char* const kMasteryRanks[kMasteryMaxLevel + 1] = { "Untrained", "Familiar", "Proficient", "Veteran", "Expert", "Master" };

struct MasteryBonus { float rate = 1.f, mag = 1.f, accuracy = 1.f; };	// accuracy < 1 = tighter
inline MasteryBonus masteryBonus(int level)
{
	if (level < 0) level = 0;
	if (level > kMasteryMaxLevel) level = kMasteryMaxLevel;
	MasteryBonus b;
	b.rate = 1.f + 0.04f * level + (level >= kMasteryMaxLevel ? 0.1f : 0.f);	// +4% a level, +10% more at Master
	b.mag = 1.f + 0.08f * level;
	b.accuracy = 1.f - 0.06f * level;
	return b;
}
inline int masteryLevelFor(int kills)
{
	int level = 0;
	while (level < kMasteryMaxLevel && kills >= kMasteryKills[level + 1]) level++;
	return level;
}

struct LootMastery
{
	int kills = 0;
	int level = 0;
	int levelKills = 0;		// kills where this level started
	int nextKills = 0;		// kills for the next level, 0 at Master
	std::string rank;		// "Veteran"
	std::vector<std::string> bonuses;	// "+12% fire rate"
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
	LootMastery mastery;
	bool rolled = true;			// false: a standard-issue gun that only has mastery bonuses
};

// one line of the mastery list in the menu
struct MasteryEntry
{
	std::string name;	// "Assault Rifle"
	LootMastery mastery;
};
