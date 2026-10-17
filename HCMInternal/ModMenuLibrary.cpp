#include "pch.h"
#include "ModMenuLibrary.h"
#include "ModMenuDefaultLibrary.h"
#include <pugixml.hpp>
#include <sstream>
#include <format>

namespace
{
	std::string trim(const std::string& s)
	{
		const char* ws = " \t\r\n";
		auto start = s.find_first_not_of(ws);
		if (start == std::string::npos) return "";
		auto end = s.find_last_not_of(ws);
		return s.substr(start, end - start + 1);
	}

	std::string toLower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return s;
	}

	void replaceAll(std::string& str, const std::string& from, const std::string& to)
	{
		if (from.empty()) return;
		size_t pos = 0;
		while ((pos = str.find(from, pos)) != std::string::npos)
		{
			str.replace(pos, from.length(), to);
			pos += to.length();
		}
	}

	std::optional<ModType> parseType(const std::string& t)
	{
		static const std::map<std::string, ModType> types =
		{
			{"toggle", ModType::Toggle},
			{"slider", ModType::Slider},
			{"button", ModType::Button},
			{"choice", ModType::Choice},
			{"text", ModType::Text},
			{"hcm-toggle", ModType::HCMToggle},
			{"hcm-slider", ModType::HCMSlider},
			{"hcm-action", ModType::HCMAction},
			{"skull", ModType::Skull},
		};
		auto it = types.find(toLower(t));
		if (it == types.end()) return std::nullopt;
		return it->second;
	}

	void appendLines(std::vector<std::string>& out, const pugi::xml_node& node)
	{
		if (!node) return;
		for (auto& line : ModMenuText::splitLines(node.text().get()))
			out.push_back(line);
	}
}

namespace ModMenuText
{
	std::set<GameState::Value> parseGames(const std::string& list)
	{
		using V = GameState::Value;
		static const std::map<std::string, std::vector<V>> tokens =
		{
			{"all", {V::Halo1, V::Halo2, V::Halo3, V::Halo3ODST, V::HaloReach, V::Halo4}},
			{"gen2up", {V::Halo2, V::Halo3, V::Halo3ODST, V::HaloReach, V::Halo4}},
			{"gen3", {V::Halo3, V::Halo3ODST, V::HaloReach, V::Halo4}},
			{"halo1", {V::Halo1}}, {"h1", {V::Halo1}}, {"ce", {V::Halo1}},
			{"halo2", {V::Halo2}}, {"h2", {V::Halo2}},
			{"halo3", {V::Halo3}}, {"h3", {V::Halo3}},
			{"halo3odst", {V::Halo3ODST}}, {"odst", {V::Halo3ODST}},
			{"haloreach", {V::HaloReach}}, {"reach", {V::HaloReach}},
			{"halo4", {V::Halo4}}, {"h4", {V::Halo4}},
		};

		std::set<V> out;
		std::stringstream ss(list);
		std::string item;
		while (std::getline(ss, item, ','))
		{
			auto key = toLower(trim(item));
			if (key.empty()) continue;
			auto it = tokens.find(key);
			if (it == tokens.end())
			{
				PLOG_ERROR << "ModMenu: unknown game token '" << key << "'";
				continue;
			}
			out.insert(it->second.begin(), it->second.end());
		}
		return out;
	}

	std::string playerExpression(GameState::Value game)
	{
		switch (game)
		{
		case GameState::Value::Halo1:
		case GameState::Value::Halo2:
			return "(unit (list_get (players) 0))";
		default:
			return "(player_get 0)";
		}
	}

	std::string expand(const std::string& command, GameState::Value game, const std::string& value)
	{
		std::string out = command;
		replaceAll(out, "{player}", playerExpression(game));
		replaceAll(out, "{players}", "(players)");
		replaceAll(out, "{value}", value);
		replaceAll(out, "{bool}", value);
		return out;
	}

	std::vector<std::string> splitLines(const std::string& block)
	{
		std::vector<std::string> out;
		std::stringstream ss(block);
		std::string line;
		while (std::getline(ss, line))
		{
			auto t = trim(line);
			if (t.empty() || t.starts_with(";")) continue;
			out.push_back(t);
		}
		return out;
	}

	std::string formatNumber(float value)
	{
		auto s = std::format("{:.3f}", value);
		// trim trailing zeros but keep at least one decimal so HaloScript reads it as a real
		while (s.size() > 2 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
		return s;
	}

	std::string shortGameName(GameState::Value game)
	{
		switch (game)
		{
		case GameState::Value::Halo1: return "CE";
		case GameState::Value::Halo2: return "H2";
		case GameState::Value::Halo3: return "H3";
		case GameState::Value::Halo3ODST: return "ODST";
		case GameState::Value::HaloReach: return "REACH";
		case GameState::Value::Halo4: return "H4";
		default: return "--";
		}
	}

	std::string longGameName(GameState::Value game)
	{
		switch (game)
		{
		case GameState::Value::Halo1: return "HALO: COMBAT EVOLVED";
		case GameState::Value::Halo2: return "HALO 2";
		case GameState::Value::Halo3: return "HALO 3";
		case GameState::Value::Halo3ODST: return "HALO 3: ODST";
		case GameState::Value::HaloReach: return "HALO: REACH";
		case GameState::Value::Halo4: return "HALO 4";
		default: return "MAIN MENU";
		}
	}
}

std::string ModLibrary::defaultXML()
{
	std::string out;
	for (size_t i = 0; i < kModMenuDefaultLibraryChunkCount; i++)
		out += kModMenuDefaultLibraryChunks[i];
	return out;
}

std::expected<ModLibrary, std::string> ModLibrary::parse(const std::string& xmlText)
{
	pugi::xml_document doc;
	auto result = doc.load_string(xmlText.c_str());
	if (!result)
		return std::unexpected(std::format("XML error at character {}: {}", (int64_t)result.offset, result.description()));

	auto root = doc.child("ModMenuLibrary");
	if (!root)
		return std::unexpected(std::string("Missing <ModMenuLibrary> root element"));

	ModLibrary lib;
	lib.version = root.attribute("version").as_int(0);
	if (auto title = std::string(root.attribute("title").as_string()); !title.empty())
		lib.title = title;

	std::set<std::string> seenIDs;

	for (auto catNode : root.children("Category"))
	{
		ModCategory cat;
		cat.id = catNode.attribute("id").as_string();
		cat.name = catNode.attribute("name").as_string(cat.id.c_str());
		cat.desc = catNode.attribute("desc").as_string();
		cat.special = toLower(catNode.attribute("special").as_string());

		for (auto modNode : catNode.children("Mod"))
		{
			ModDef mod;
			mod.id = modNode.attribute("id").as_string();
			mod.name = modNode.attribute("name").as_string(mod.id.c_str());
			mod.desc = modNode.attribute("desc").as_string();
			mod.tag = modNode.attribute("tag").as_string();
			mod.hcmKey = modNode.attribute("hcm").as_string();
			mod.skullKey = modNode.attribute("skull").as_string();
			mod.reapply = modNode.attribute("reapply").as_bool(false);
			mod.minValue = modNode.attribute("min").as_float(0.f);
			mod.maxValue = modNode.attribute("max").as_float(1.f);
			mod.step = modNode.attribute("step").as_float(0.1f);
			mod.defaultValue = modNode.attribute("default").as_float(mod.minValue);

			if (mod.id.empty())
				return std::unexpected(std::format("A mod in category '{}' has no id", cat.name));
			if (seenIDs.contains(mod.id))
				return std::unexpected(std::format("Duplicate mod id '{}'", mod.id));
			seenIDs.insert(mod.id);

			auto type = parseType(modNode.attribute("type").as_string("button"));
			if (!type)
				return std::unexpected(std::format("Mod '{}' has unknown type '{}'", mod.id, modNode.attribute("type").as_string()));
			mod.type = type.value();

			if (mod.maxValue <= mod.minValue) mod.maxValue = mod.minValue + 1.f;
			if (mod.step <= 0.f) mod.step = 0.01f;

			// games declared on the mod itself (used by HCM bridges and skulls)
			mod.games = ModMenuText::parseGames(modNode.attribute("games").as_string());

			for (auto gameNode : modNode.children("Game"))
			{
				auto games = ModMenuText::parseGames(gameNode.attribute("games").as_string());
				for (auto g : games)
				{
					auto& cmds = mod.perGame[g];
					appendLines(cmds.on, gameNode.child("On"));
					appendLines(cmds.off, gameNode.child("Off"));
					appendLines(cmds.set, gameNode.child("Set"));
					appendLines(cmds.apply, gameNode.child("Apply"));
					appendLines(cmds.run, gameNode.child("Run"));
					mod.games.insert(g);
				}
			}

			for (auto optNode : modNode.children("Option"))
			{
				ModChoiceOption opt;
				opt.label = optNode.attribute("label").as_string();
				opt.value = optNode.attribute("value").as_string(opt.label.c_str());
				opt.games = ModMenuText::parseGames(optNode.attribute("games").as_string());
				mod.options.push_back(opt);
			}

			if (mod.type == ModType::Choice && mod.options.empty())
				return std::unexpected(std::format("Choice mod '{}' has no <Option> entries", mod.id));

			if ((mod.type == ModType::HCMToggle || mod.type == ModType::HCMSlider || mod.type == ModType::HCMAction) && mod.hcmKey.empty())
				return std::unexpected(std::format("Mod '{}' needs an hcm=\"...\" attribute", mod.id));

			if (mod.type == ModType::Skull && mod.skullKey.empty())
				return std::unexpected(std::format("Skull mod '{}' needs a skull=\"...\" attribute", mod.id));

			cat.mods.push_back(std::move(mod));
		}

		lib.categories.push_back(std::move(cat));
	}

	if (lib.categories.empty())
		return std::unexpected(std::string("The library has no categories"));

	return lib;
}
