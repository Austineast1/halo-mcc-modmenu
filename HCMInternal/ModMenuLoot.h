#pragma once
#include "pch.h"
#include "GameState.h"
#include "DIContainer.h"
#include "ModMenuLootTypes.h"

// Borderlands-style weapon loot for Halo: Reach (MCC 1.3528).
//
// Every handheld gun the level has loaded gets a rarity, a manufacturer and a few perks, written straight into the
// weapon's tag data in memory: fire rate, rounds per shot, spread, magazine size, and which projectile it fires
// (an "element", picked from projectiles other guns on the level already fire, so it's always loaded).
// Nothing on disk changes. Every original value is kept and written back by restore(); a new level reloads the
// tags anyway. Tags are shared, so enemies carrying the same gun get the same roll.
//
// Weapon tag layout (from the ReachMCC plugin, checked live Oct 2026): magazines block +0x3F0 (0x60 each:
// rounds total initial +0x6, total maximum +0x8, loaded maximum +0xA), barrels block +0x408 (0x184 each:
// rounds per second +0x4/+0x8, fire recovery +0x28, projectiles per shot +0x6A, minimum error +0x70,
// error angle +0x74/+0x78, projectile reference +0x104 with its datum at +0x110).
class ModMenuLoot
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	// throws HCMInitException if this game / MCC version isn't supported
	ModMenuLoot(GameState game, IDIContainer& dicon);
	~ModMenuLoot();

	// Render thread. Rolls every gun on the level (a new random seed unless one is given). Returns a footer message.
	std::string roll(std::optional<uint32_t> seed = std::nullopt);
	// Render thread. Puts every gun back to how the level shipped it.
	std::string restore();

	const std::vector<LootWeapon>& weapons() const;	// the current roll; empty when loot is off
	bool active() const;
	uint32_t seed() const;

	// roll fresh loot every time a mission loads
	void setAutoReroll(bool on);
	bool autoReroll() const;

	// Render thread, every frame: does the automatic re-roll a few seconds after a level loads. Returns a message once.
	std::optional<std::string> update(bool gamePlaying);

	void onLevelChanged();	// any thread: the tags were reloaded, so nothing we wrote is there any more

	// Render thread: the rolled gun the player is holding right now (for the item card), or null.
	const LootWeapon* heldWeapon();

	// Render thread. Generates a brand-new roll for one gun type (at least minRarity: 0 common .. 4 legendary) and
	// returns its card; every other gun keeps its roll. Rolls loot for the level first if it's off.
	std::optional<LootWeapon> regenerate(uint32_t tag, int minRarity);
	uint32_t heldGunTag();	// the gun type in the player's hands, or 0xFFFFFFFF
	std::vector<std::pair<uint32_t, std::string>> gunTypes() const;	// every gun type on the level (tag, name)

	// developer: one line per gun with its current stats (read-only)
	std::vector<std::string> describeGuns();

	// ---- weapon mastery: kills with a gun type level it up (saved in ModMenuMastery.txt), each level makes it better.
	// Kills are found by watching bipeds die and reading who dealt the last damage and with what (object +0xF0 damage
	// owner, +0x154 last damage effect, +0x138 bit 2 dead; from haloreach.dll 1.3528, object_set_damage_owner).
	struct MasteryKill
	{
		uint32_t tag = 0xFFFFFFFF;
		std::string name;
		LootMastery mastery;	// after this kill
		bool levelUp = false;
		int64_t atMs = 0;		// steady clock
	};
	const MasteryKill* lastKill() const;	// the most recent credited kill, or null
	std::vector<MasteryEntry> masteryList();	// every gun type with kills, most kills first
	std::string addMastery(int kills);		// developer: credit the held gun with some kills
	std::string resetMastery(bool all);		// developer: forget the held gun's kills (or every gun's)
	std::vector<std::string> recentDeaths() const;	// developer: the last few bipeds seen dying and who hit them last

	// item cards: a rolled gun, or a standard-issue gun with mastery progress
	bool cardsAvailable() const;
	uint32_t cardVersion() const;			// changes whenever the cards are rebuilt (roll, restore, level up)
	bool levelUpCard(uint32_t tag) const;	// this gun's card is showing because it just levelled up
};
