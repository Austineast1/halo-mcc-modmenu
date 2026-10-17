#pragma once
#include "pch.h"
#include "GameState.h"

// Data model for the in-game Mod Menu.
// Mods are defined in ModMenuLibrary.xml (a copy is compiled in as the default, see ModMenuDefaultLibrary.h).
// Most mods are HaloScript command templates sent through MCC's own engine command interface (GameEngineFunctions::SendCommand),
// so they keep working across MCC patches without any per-version memory offsets.

enum class ModType
{
	Toggle,
	Slider,
	Button,
	Choice,
	Text,
	HCMToggle,
	HCMSlider,
	HCMAction,
	Skull
};

struct ModCommands
{
	std::vector<std::string> on;
	std::vector<std::string> off;
	std::vector<std::string> set;	// toggle template using {bool}
	std::vector<std::string> apply;	// slider/choice/text template using {value}
	std::vector<std::string> run;	// button
};

struct ModChoiceOption
{
	std::string label;
	std::string value;
	std::set<GameState::Value> games; // empty = every game
	bool supports(GameState::Value g) const { return games.empty() || games.contains(g); }
};

struct ModDef
{
	std::string id;
	std::string name;
	std::string desc;
	std::string tag;
	std::string hcmKey;		// for HCMToggle / HCMSlider / HCMAction
	std::string skullKey;	// for Skull
	ModType type = ModType::Button;
	float minValue = 0.f;
	float maxValue = 1.f;
	float step = 0.1f;
	float defaultValue = 0.f;
	bool reapply = false;
	std::string group;		// buttons sharing a group are one-at-a-time (e.g. Become forms); the last one run is remembered
	std::vector<ModChoiceOption> options;
	std::map<GameState::Value, ModCommands> perGame;
	std::set<GameState::Value> games; // every game this mod appears in

	bool supports(GameState::Value g) const { return games.contains(g); }
	const ModCommands* commandsFor(GameState::Value g) const
	{
		auto it = perGame.find(g);
		return it == perGame.end() ? nullptr : &it->second;
	}
};

struct ModCategory
{
	std::string id;
	std::string name;
	std::string desc;
	std::string special; // "skulls" renders the live skull panel instead of mods
	std::vector<ModDef> mods;
};

struct ModLibrary
{
	int version = 0;
	std::string title = "MOD MENU";
	std::vector<ModCategory> categories;

	// Parses library xml. Returns an error string on failure.
	static std::expected<ModLibrary, std::string> parse(const std::string& xmlText);

	// The library compiled into HCMInternal.
	static std::string defaultXML();
};

namespace ModMenuText
{
	// Parses "Halo1,Halo3" / "all" / "gen3" / "gen2up" into a set of games.
	std::set<GameState::Value> parseGames(const std::string& list);

	// Replaces {player} {players} {value} {bool} for a given game.
	std::string expand(const std::string& command, GameState::Value game, const std::string& value);

	// Splits a multi-line block into trimmed commands, skipping blanks and ; comments.
	std::vector<std::string> splitLines(const std::string& block);

	std::string formatNumber(float value);

	std::string shortGameName(GameState::Value game);
	std::string longGameName(GameState::Value game);
}
