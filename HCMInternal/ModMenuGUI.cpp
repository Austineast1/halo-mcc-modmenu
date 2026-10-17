#include "pch.h"
#include "ModMenuGUI.h"
#include "SkullEnum.h"
#include "ScopedImFontScaler.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <shellapi.h>
#include <format>
#include <sstream>
#include <deque>

namespace
{
	constexpr ImVec4 rgba(int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }

	// palette: dark slate with a visor-amber accent and telemetry cyan
	constexpr ImVec4 colWindow = rgba(9, 12, 17, 0.96f);
	constexpr ImVec4 colPanel = rgba(14, 19, 26);
	constexpr ImVec4 colBorder = rgba(64, 80, 100, 0.55f);
	constexpr ImVec4 colFrame = rgba(22, 29, 38);
	constexpr ImVec4 colFrameHover = rgba(34, 44, 57);
	constexpr ImVec4 colText = rgba(222, 230, 240);
	constexpr ImVec4 colDim = rgba(128, 142, 160);
	constexpr ImVec4 colFaint = rgba(78, 90, 106);
	constexpr ImVec4 colAccent = rgba(242, 170, 60);
	constexpr ImVec4 colAccentDim = rgba(242, 170, 60, 0.35f);
	constexpr ImVec4 colCyan = rgba(90, 220, 255);
	constexpr ImVec4 colGood = rgba(120, 240, 160);
	constexpr ImVec4 colBad = rgba(255, 96, 110);
	constexpr ImVec4 colTag = rgba(255, 120, 190);

	std::string timestamp()
	{
		using namespace std::chrono;
		auto now = system_clock::now();
		auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
		std::time_t t = system_clock::to_time_t(now);
		std::tm local{};
		localtime_s(&local, &t);
		return std::format("{:02}:{:02}:{:02}.{:03}", local.tm_hour, local.tm_min, local.tm_sec, (int)ms.count());
	}

	// right edge of the content area, measured from the start of the current line
	float lineRightEdge() { return ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x; }

	std::string lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return s;
	}

	const std::map<SkullEnum, const char*>& skullDescriptions()
	{
		static const std::map<SkullEnum, const char*> d =
		{
			{SkullEnum::Anger, "Enemies fire faster and more often."},
			{SkullEnum::Assassins, "Every enemy is permanently cloaked."},
			{SkullEnum::BlackEye, "Shields only recharge when you melee enemies."},
			{SkullEnum::Blind, "No HUD and no weapon on screen."},
			{SkullEnum::Catch, "Enemies throw and drop far more grenades."},
			{SkullEnum::EyePatch, "No aim assist."},
			{SkullEnum::Famine, "Dropped weapons carry half ammo."},
			{SkullEnum::Fog, "No motion tracker."},
			{SkullEnum::Foreign, "You can't use the other side's weapons."},
			{SkullEnum::Iron, "Dying sends you back to the start or your last checkpoint."},
			{SkullEnum::Jacked, "Ground vehicles can only be entered by boarding."},
			{SkullEnum::Masterblaster, "Co-op role swap between overshield melee and infinite ammo."},
			{SkullEnum::Mythic, "Enemies have more health."},
			{SkullEnum::Recession, "Every shot costs double ammo."},
			{SkullEnum::SoAngry, "Enraged Brutes explode if you don't kill them fast."},
			{SkullEnum::Streaking, "Shields drain over time but refill on kills."},
			{SkullEnum::Swarm, "Hunters are much stronger."},
			{SkullEnum::ThatsJustWrong, "Enemies notice you faster."},
			{SkullEnum::TheyComeBack, "Reanimated Flood are far more dangerous."},
			{SkullEnum::Thunderstorm, "Most enemies are promoted a rank."},
			{SkullEnum::Tilt, "Enemy resistances and weaknesses are stronger."},
			{SkullEnum::ToughLuck, "Enemies always dodge, always go berserk, never flee."},
			{SkullEnum::Bandanna, "Infinite ammo, grenades and abilities."},
			{SkullEnum::BondedPair, "Co-op: when your partner dies you get a damage boost."},
			{SkullEnum::Boom, "Explosion radius doubled."},
			{SkullEnum::Cowbell, "Explosions throw things much harder."},
			{SkullEnum::Envy, "Flashlight swapped for active camo."},
			{SkullEnum::Feather, "Melee hits send things flying."},
			{SkullEnum::Ghost, "AI never flinch."},
			{SkullEnum::GruntBirthdayParty, "Grunt headshots pop confetti."},
			{SkullEnum::GruntFuneral, "Grunts explode in plasma when they die."},
			{SkullEnum::IWHBYD, "Rare combat dialogue is common."},
			{SkullEnum::Malfunction, "A random part of your HUD breaks on respawn."},
			{SkullEnum::Pinata, "Punched enemies drop grenades."},
			{SkullEnum::ProphetBirthdayParty, "Prophet surprise."},
			{SkullEnum::Scarab, "Every weapon you hold fires a Scarab beam."},
			{SkullEnum::Sputnik, "Objects are lighter and fly further."},
			{SkullEnum::Acrophobia, "Infinite jetpack flight."},
		};
		return d;
	}
}

ModMenuGUI::ModMenuGUI(GameState game, std::shared_ptr<GameEngineFunctions> engine, std::weak_ptr<SettingsStateAndEvents> settings, std::string dirPath)
	: mGame(game), mEngine(engine), mSettingsWeak(settings)
{
	if (!dirPath.empty() && dirPath.back() != '\\' && dirPath.back() != '/') dirPath += "\\";
	mLibraryPath = dirPath + "ModMenuLibrary.xml";
	loadLibrary();
}

// ------------------------------------------------------------------ library

void ModMenuGUI::loadLibrary()
{
	std::string builtInText = ModLibrary::defaultXML();
	auto builtIn = ModLibrary::parse(builtInText);
	if (!builtIn)
	{
		mLibrary = ModLibrary{};
		mLibraryStatus = "Built-in library failed to load: " + builtIn.error();
		mLibraryStatusIsError = true;
		addLog(mLibraryStatus, LogLine::Kind::Error);
		return;
	}

	auto writeBuiltIn = [&]() -> bool
		{
			try
			{
				std::ofstream out(mLibraryPath, std::ios::binary | std::ios::trunc);
				out << builtInText;
				return out.good();
			}
			catch (...) { return false; }
		};

	mLibraryStatusIsError = false;

	if (std::filesystem::exists(mLibraryPath))
	{
		std::ifstream in(mLibraryPath, std::ios::binary);
		std::stringstream buffer;
		buffer << in.rdbuf();
		auto fromFile = ModLibrary::parse(buffer.str());

		if (!fromFile)
		{
			mLibrary = builtIn.value();
			mLibraryStatus = std::format("ModMenuLibrary.xml has an error ({}). Using the built-in library.", fromFile.error());
			mLibraryStatusIsError = true;
		}
		else if (fromFile->version < builtIn->version)
		{
			// ship newer built-in mods, but keep the user's old file
			auto backup = mLibraryPath.substr(0, mLibraryPath.size() - 4) + std::format(".backup-v{}.xml", fromFile->version);
			try { std::filesystem::copy_file(mLibraryPath, backup, std::filesystem::copy_options::overwrite_existing); }
			catch (...) {}
			writeBuiltIn();
			mLibrary = builtIn.value();
			mLibraryStatus = std::format("Library updated to v{}. Your old file was saved next to it.", builtIn->version);
		}
		else
		{
			mLibrary = fromFile.value();
			mLibraryStatus = std::format("Loaded ModMenuLibrary.xml v{}", mLibrary.version);
		}
	}
	else
	{
		mLibrary = builtIn.value();
		mLibraryStatus = writeBuiltIn()
			? std::format("Created ModMenuLibrary.xml v{} next to HCM. Edit it to add your own mods.", mLibrary.version)
			: std::string("Couldn't write ModMenuLibrary.xml, using the built-in library.");
	}

	// set up runtime values for any mods we haven't seen yet
	for (auto& cat : mLibrary.categories)
	{
		for (auto& mod : cat.mods)
		{
			if (mState.contains(mod.id)) continue;
			auto& rt = mState[mod.id];
			rt.value = std::clamp(mod.defaultValue, mod.minValue, mod.maxValue);
			if (mod.type == ModType::HCMSlider)
			{
				if (auto s = hcmDouble(mod.hcmKey)) rt.value = (float)s->GetValue();
			}
		}
	}

	if (mSelectedCategory >= (int)mLibrary.categories.size()) mSelectedCategory = 0;
	addLog(mLibraryStatus, mLibraryStatusIsError ? LogLine::Kind::Error : LogLine::Kind::Info);
}

// ------------------------------------------------------------------ logging

void ModMenuGUI::addLog(const std::string& text, LogLine::Kind kind)
{
	std::lock_guard<std::mutex> lock(mLogMutex);
	mLog.push_back(LogLine{ timestamp(), text, kind });
	while (mLog.size() > mMaxLog) mLog.pop_front();
	mScrollLogToBottom = true;
}

void ModMenuGUI::onHaloScriptOutput(const std::string& outputString, const HSOutputType& outputType)
{
	addLog(outputString, outputType == HSOutputType::Error ? LogLine::Kind::Error : LogLine::Kind::Output);
}

// ------------------------------------------------------------------ execution

void ModMenuGUI::sendLines(const std::vector<std::string>& lines, const std::string& value)
{
	for (auto& line : lines)
	{
		auto command = ModMenuText::expand(line, mGame, value);
		addLog(command, LogLine::Kind::Command);
		mCommandsSent++;
		try
		{
			mEngine->SendCommand(command.c_str());
		}
		catch (HCMRuntimeException ex)
		{
			addLog(std::format("HCM error: {}", ex.what()), LogLine::Kind::Error);
		}
	}
}

ModMenuGUI::ModRuntime& ModMenuGUI::runtime(const ModDef& mod)
{
	return mState[mod.id];
}

void ModMenuGUI::applyToggle(const ModDef& mod, bool on)
{
	auto cmds = mod.commandsFor(mGame);
	if (!cmds) return;
	auto& rt = runtime(mod);

	if (on)
	{
		if (!cmds->on.empty()) sendLines(cmds->on, "true");
		else sendLines(cmds->set, "true");
	}
	else
	{
		if (!cmds->off.empty()) sendLines(cmds->off, "false");
		else sendLines(cmds->set, "false");
	}

	bool canTurnOff = !cmds->off.empty() || !cmds->set.empty();
	rt.on = canTurnOff ? on : false;
}

void ModMenuGUI::applySlider(const ModDef& mod, float value)
{
	auto cmds = mod.commandsFor(mGame);
	if (!cmds) return;
	auto& rt = runtime(mod);
	rt.value = std::clamp(value, mod.minValue, mod.maxValue);
	sendLines(cmds->apply, ModMenuText::formatNumber(rt.value));
	bool hasDefault = mod.defaultValue >= mod.minValue && mod.defaultValue <= mod.maxValue;
	rt.applied = !hasDefault || std::fabs(rt.value - mod.defaultValue) > mod.step * 0.5f;
}

void ModMenuGUI::applyChoice(const ModDef& mod, int optionIndex)
{
	auto cmds = mod.commandsFor(mGame);
	if (!cmds || optionIndex < 0 || optionIndex >= (int)mod.options.size()) return;
	auto& rt = runtime(mod);
	if (rt.applied && rt.option != optionIndex && !cmds->off.empty())
		sendLines(cmds->off, mod.options[rt.option].value); // undo the previous pick first
	rt.option = optionIndex;
	sendLines(cmds->apply, mod.options[optionIndex].value);
	rt.applied = true;
}

void ModMenuGUI::undoChoice(const ModDef& mod)
{
	auto cmds = mod.commandsFor(mGame);
	auto& rt = runtime(mod);
	if (!cmds || !rt.applied) return;
	if (!cmds->off.empty()) sendLines(cmds->off, mod.options[rt.option].value);
	rt.applied = false;
}

void ModMenuGUI::runButton(const ModDef& mod)
{
	auto cmds = mod.commandsFor(mGame);
	if (!cmds) return;
	sendLines(cmds->run, "");
}

void ModMenuGUI::runText(const ModDef& mod, const std::string& text)
{
	auto cmds = mod.commandsFor(mGame);
	if (!cmds || text.empty()) return;
	sendLines(cmds->apply, text);
}

int ModMenuGUI::reapplyActive(const std::string& reason)
{
	int count = 0;
	for (auto& cat : mLibrary.categories)
	{
		for (auto& mod : cat.mods)
		{
			if (!mod.reapply || !mod.supports(mGame)) continue;
			auto& rt = runtime(mod);
			auto cmds = mod.commandsFor(mGame);
			if (!cmds) continue;

			switch (mod.type)
			{
			case ModType::Toggle:
				if (rt.on) { applyToggle(mod, true); count++; }
				break;
			case ModType::Slider:
				if (rt.applied) { applySlider(mod, rt.value); count++; }
				break;
			case ModType::Choice:
				if (rt.applied) { sendLines(cmds->apply, mod.options[rt.option].value); count++; }
				break;
			default:
				break;
			}
		}
	}
	if (count > 0) addLog(std::format("Re-applied {} mod{} after {}", count, count == 1 ? "" : "s", reason), LogLine::Kind::Info);
	return count;
}

bool ModMenuGUI::hasActiveReapplyMods()
{
	for (auto& cat : mLibrary.categories)
		for (auto& mod : cat.mods)
			if (mod.reapply && mod.supports(mGame) && isActive(mod)) return true;
	return false;
}

void ModMenuGUI::turnEverythingOff()
{
	for (auto& cat : mLibrary.categories)
	{
		for (auto& mod : cat.mods)
		{
			if (!mod.supports(mGame)) continue;
			auto& rt = runtime(mod);
			switch (mod.type)
			{
			case ModType::Toggle:
				if (rt.on) applyToggle(mod, false);
				break;
			case ModType::Slider:
				if (rt.applied)
				{
					if (mod.defaultValue >= mod.minValue && mod.defaultValue <= mod.maxValue) applySlider(mod, mod.defaultValue);
					else rt.applied = false;
				}
				break;
			case ModType::Choice:
				if (rt.applied) undoChoice(mod);
				break;
			case ModType::HCMToggle:
				if (hcmBoolValue(mod)) setHcmBool(mod, false);
				break;
			default:
				break;
			}
		}
	}
	addLog("Turned every mod off", LogLine::Kind::Info);
}

// ------------------------------------------------------------------ HCM bridges

std::shared_ptr<BinarySetting<bool>> ModMenuGUI::hcmBool(const std::string& key)
{
	auto s = mSettingsWeak.lock();
	if (!s) return nullptr;
	if (key == "invulnerability") return s->invulnerabilityToggle;
	if (key == "invulnerabilityNPC") return s->invulnerabilityNPCToggle;
	if (key == "aiFreeze") return s->aiFreezeToggle;
	if (key == "medusa") return s->medusaToggle;
	if (key == "infiniteAmmo") return s->infiniteAmmoToggle;
	if (key == "bottomlessClip") return s->bottomlessClipToggle;
	if (key == "speedhack") return s->speedhackToggle;
	if (key == "disableBarriers") return s->disableBarriersToggle;
	if (key == "hideHUD") return s->hideHUDToggle;
	if (key == "freeCamera") return s->freeCameraToggle;
	if (key == "disableFog") return s->disableFogToggle;
	if (key == "naturalCheckpointDisable") return s->naturalCheckpointDisable;
	if (key == "display2DInfo") return s->display2DInfoToggle;
	if (key == "togglePause") return s->togglePause;
	return nullptr;
}

std::shared_ptr<BinarySetting<double>> ModMenuGUI::hcmDouble(const std::string& key)
{
	auto s = mSettingsWeak.lock();
	if (!s) return nullptr;
	if (key == "speedhackValue") return s->speedhackSetting;
	return nullptr;
}

std::shared_ptr<ActionEvent> ModMenuGUI::hcmAction(const std::string& key)
{
	auto s = mSettingsWeak.lock();
	if (!s) return nullptr;
	if (key == "forceCheckpoint") return s->forceCheckpointEvent;
	if (key == "forceRevert") return s->forceRevertEvent;
	if (key == "forceDoubleRevert") return s->forceDoubleRevertEvent;
	if (key == "forceMissionRestart") return s->forceMissionRestartEvent;
	if (key == "forceTeleport") return s->forceTeleportEvent;
	if (key == "forceLaunch") return s->forceLaunchEvent;
	if (key == "setPlayerHealth") return s->setPlayerHealthEvent;
	if (key == "injectCheckpoint") return s->injectCheckpointEvent;
	if (key == "dumpCheckpoint") return s->dumpCheckpointEvent;
	return nullptr;
}

bool ModMenuGUI::hcmBoolValue(const ModDef& mod)
{
	auto setting = hcmBool(mod.hcmKey);
	return setting ? setting->GetValue() : false;
}

void ModMenuGUI::setHcmBool(const ModDef& mod, bool value)
{
	auto setting = hcmBool(mod.hcmKey);
	if (!setting)
	{
		addLog(std::format("Unknown HCM toggle '{}'", mod.hcmKey), LogLine::Kind::Error);
		return;
	}
	setting->GetValueDisplay() = value;
	setting->UpdateValueWithInput();
	addLog(std::format("HCM {} = {}", mod.hcmKey, value ? "on" : "off"), LogLine::Kind::Info);
}

// ------------------------------------------------------------------ skulls

std::map<SkullEnum, bool> ModMenuGUI::readSkulls()
{
	std::map<SkullEnum, bool> out;
	auto s = mSettingsWeak.lock();
	if (!s) return out;

	try
	{
		s->updateSkullBitBoolCollectionEvent->operator()(mGame);
		ScopedAtomicBool lock(s->skullBitBoolCollectionInUse);
		for (auto& [skull, pointer] : s->skullBitBoolCollection)
			out[skull] = pointer.operator const bool();
	}
	catch (HCMRuntimeException ex)
	{
		addLog(std::format("Skull read failed: {}", ex.what()), LogLine::Kind::Error);
	}
	return out;
}

void ModMenuGUI::flipSkull(SkullEnum skull)
{
	auto s = mSettingsWeak.lock();
	if (!s) return;
	try
	{
		ScopedAtomicBool lock(s->skullBitBoolCollectionInUse);
		auto it = s->skullBitBoolCollection.find(skull);
		if (it == s->skullBitBoolCollection.end()) return;
		bool previous = it->second.operator const bool();
		it->second.set(!previous);
		addLog(std::format("Skull {} {}", magic_enum::enum_name(skull), previous ? "off" : "on"), LogLine::Kind::Info);
	}
	catch (HCMRuntimeException ex)
	{
		addLog(std::format("Skull toggle failed: {}", ex.what()), LogLine::Kind::Error);
	}
}

// ------------------------------------------------------------------ state helpers

bool ModMenuGUI::isActive(const ModDef& mod)
{
	auto& rt = runtime(mod);
	switch (mod.type)
	{
	case ModType::Toggle: return rt.on;
	case ModType::Slider: return rt.applied;
	case ModType::Choice: return rt.applied;
	case ModType::HCMToggle: return hcmBoolValue(mod);
	default: return false;
	}
}

int ModMenuGUI::activeCount()
{
	int n = 0;
	for (auto& cat : mLibrary.categories)
		for (auto& mod : cat.mods)
			if (mod.supports(mGame) && isActive(mod)) n++;
	return n;
}

bool ModMenuGUI::matchesSearch(const ModDef& mod) const
{
	if (mSearch.empty()) return true;
	auto q = lower(mSearch);
	return lower(mod.name).contains(q) || lower(mod.desc).contains(q) || lower(mod.id).contains(q) || lower(mod.tag).contains(q);
}

std::vector<const ModDef*> ModMenuGUI::visibleMods(const ModCategory& cat)
{
	std::vector<const ModDef*> out;
	std::map<SkullEnum, bool> skulls;
	bool skullsRead = false;

	for (auto& mod : cat.mods)
	{
		if (!mod.supports(mGame) || !matchesSearch(mod)) continue;

		if (mod.type == ModType::Skull)
		{
			if (!skullsRead) { skulls = readSkulls(); skullsRead = true; }
			auto skull = magic_enum::enum_cast<SkullEnum>(mod.skullKey);
			if (!skull || !skulls.contains(skull.value())) continue; // this game doesn't have it
		}
		out.push_back(&mod);
	}
	return out;
}

// ------------------------------------------------------------------ rendering

void ModMenuGUI::pushStyle()
{
	ImGui::PushStyleColor(ImGuiCol_WindowBg, colWindow);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, colPanel);
	ImGui::PushStyleColor(ImGuiCol_Border, colBorder);
	ImGui::PushStyleColor(ImGuiCol_Text, colText);
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, colDim);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, colFrame);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colFrameHover);
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colFrameHover);
	ImGui::PushStyleColor(ImGuiCol_Button, colFrame);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colFrameHover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, colAccentDim);
	ImGui::PushStyleColor(ImGuiCol_CheckMark, colAccent);
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, colAccent);
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, colCyan);
	ImGui::PushStyleColor(ImGuiCol_Header, colAccentDim);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colFrameHover);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, colAccentDim);
	ImGui::PushStyleColor(ImGuiCol_Separator, colBorder);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, colPanel);
	ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, colWindow);
	ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, colFrameHover);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 2.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 5.f));
}

void ModMenuGUI::popStyle()
{
	ImGui::PopStyleVar(5);
	ImGui::PopStyleColor(21);
}

void ModMenuGUI::renderTag(const std::string& tag)
{
	if (tag.empty()) return;
	ImGui::SameLine();
	ImGui::TextColored(colTag, "[%s]", tag.c_str());
}

void ModMenuGUI::renderGameChips(const ModDef& mod, float right)
{
	using V = GameState::Value;
	static const V order[] = { V::Halo1, V::Halo2, V::Halo3, V::Halo3ODST, V::HaloReach, V::Halo4 };

	std::string all;
	for (auto g : order) all += ModMenuText::shortGameName(g) + " ";
	float width = ImGui::CalcTextSize(all.c_str()).x;

	ImGui::SameLine();
	float x = std::max(ImGui::GetCursorPosX(), right - width);
	ImGui::SetCursorPosX(x);

	bool first = true;
	for (auto g : order)
	{
		if (!first) ImGui::SameLine(0.f, ImGui::CalcTextSize(" ").x);
		first = false;
		auto name = ModMenuText::shortGameName(g);
		ImVec4 c = !mod.supports(g) ? colFaint : (g == mGame.operator GameState::Value() ? colAccent : colDim);
		ImGui::TextColored(c, "%s", name.c_str());
	}
}

void ModMenuGUI::renderHeader()
{
	float right = lineRightEdge();
	{
		ScopedImFontScaler big(mFontSize * 1.45f);
		ImGui::TextColored(colAccent, "%s", mLibrary.title.c_str());
	}
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextColored(colDim, " //  %s  //  %d active  //  %d sent",
		ModMenuText::longGameName(mGame).c_str(), activeCount(), mCommandsSent);

	float closeW = ImGui::CalcTextSize("CLOSE [Insert]").x + ImGui::GetStyle().FramePadding.x * 2.f;
	ImGui::SameLine(right - closeW);
	if (ImGui::Button("CLOSE [Insert]")) mCloseRequested = true;

	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputTextWithHint("##modsearch", "Search every mod (camo, gravity, hunter, skull...)", &mSearch);
	if (mGame.operator GameState::Value() == GameState::Value::Halo4)
		ImGui::TextColored(colTag, "Halo 4 has no published script list, so some scripted mods may print an error and do nothing.");
}

void ModMenuGUI::renderCategoryList()
{
	for (int i = 0; i < (int)mLibrary.categories.size(); i++)
	{
		auto& cat = mLibrary.categories[i];
		int total = 0, active = 0;

		if (cat.special == "skulls")
		{
			auto skulls = readSkulls();
			total = (int)skulls.size();
			for (auto& [k, v] : skulls) if (v) active++;
		}
		else
		{
			for (auto* mod : visibleMods(cat))
			{
				total++;
				if (isActive(*mod)) active++;
			}
		}

		if (total == 0 && mSearch.empty() == false) continue;

		bool selected = mSelectedCategory == i && mSearch.empty();
		float right = lineRightEdge();
		auto label = std::format("{}##cat{}", cat.name, i);
		ImGui::PushStyleColor(ImGuiCol_Text, total == 0 ? colFaint : (selected ? colAccent : colText));
		if (ImGui::Selectable(label.c_str(), selected))
		{
			mSelectedCategory = i;
			mSearch.clear();
		}
		ImGui::PopStyleColor();

		auto count = active > 0 ? std::format("{}/{}", active, total) : std::format("{}", total);
		ImGui::SameLine(right - ImGui::CalcTextSize(count.c_str()).x);
		ImGui::TextColored(active > 0 ? colGood : colFaint, "%s", count.c_str());
	}
}

void ModMenuGUI::renderModList()
{
	if (mLibrary.categories.empty())
	{
		ImGui::TextColored(colBad, "No mods loaded. Check the status line below.");
		return;
	}

	if (!mSearch.empty())
	{
		int found = 0;
		for (auto& cat : mLibrary.categories)
		{
			auto mods = visibleMods(cat);
			if (mods.empty()) continue;
			ImGui::TextColored(colAccent, "%s", cat.name.c_str());
			ImGui::Separator();
			for (auto* mod : mods) { renderMod(*mod); found++; }
			ImGui::Dummy(ImVec2(0.f, 6.f));
		}
		if (found == 0) ImGui::TextColored(colDim, "Nothing matches \"%s\" in this game.", mSearch.c_str());
		return;
	}

	auto& cat = mLibrary.categories[mSelectedCategory];
	ImGui::TextColored(colAccent, "%s", cat.name.c_str());
	ImGui::TextColored(colDim, "%s", cat.desc.c_str());
	ImGui::Separator();

	if (cat.special == "skulls")
	{
		renderSkullPanel();
		return;
	}

	auto mods = visibleMods(cat);
	if (mods.empty())
	{
		ImGui::TextColored(colDim, "Nothing in this tab works in %s.", ModMenuText::longGameName(mGame).c_str());
		ImGui::TextColored(colFaint, "Switch games in MCC to see these mods.");
		return;
	}

	for (auto* mod : mods) renderMod(*mod);
}

void ModMenuGUI::renderMod(const ModDef& mod)
{
	ImGui::PushID(mod.id.c_str());
	auto& rt = runtime(mod);
	auto cmds = mod.commandsFor(mGame);
	bool active = isActive(mod);
	float rowWidth = ImGui::GetContentRegionAvail().x;
	float rowRight = lineRightEdge();

	auto nameLine = [&]()
		{
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(active ? colGood : colText, "%s", mod.name.c_str());
			renderTag(mod.tag);
			if (mod.reapply)
			{
				ImGui::SameLine();
				ImGui::TextColored(colFaint, "(r)");
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-applied automatically after a checkpoint revert or level load.");
			}
			renderGameChips(mod, rowRight);
		};

	switch (mod.type)
	{
	case ModType::Toggle:
	{
		bool canTurnOff = cmds && (!cmds->off.empty() || !cmds->set.empty());
		if (canTurnOff)
		{
			bool v = rt.on;
			if (ImGui::Checkbox("##toggle", &v)) applyToggle(mod, v);
		}
		else
		{
			if (ImGui::Button("FIRE")) applyToggle(mod, true);
		}
		nameLine();
		break;
	}
	case ModType::Button:
	{
		if (ImGui::Button("RUN ")) runButton(mod);
		nameLine();
		break;
	}
	case ModType::HCMToggle:
	{
		bool v = hcmBoolValue(mod);
		if (ImGui::Checkbox("##hcmtoggle", &v)) setHcmBool(mod, v);
		nameLine();
		break;
	}
	case ModType::HCMAction:
	{
		if (ImGui::Button("RUN "))
		{
			if (mod.hcmKey == "commandConsole")
			{
				mOpenConsoleRequested = true;
			}
			else if (auto ev = hcmAction(mod.hcmKey))
			{
				addLog(std::format("HCM action {}", mod.hcmKey), LogLine::Kind::Info);
				ev->operator()();
			}
			else
			{
				addLog(std::format("Unknown HCM action '{}'", mod.hcmKey), LogLine::Kind::Error);
			}
		}
		nameLine();
		break;
	}
	case ModType::Skull:
	{
		auto skull = magic_enum::enum_cast<SkullEnum>(mod.skullKey);
		auto skulls = readSkulls();
		bool v = skull && skulls.contains(skull.value()) ? skulls.at(skull.value()) : false;
		bool before = v;
		if (ImGui::Checkbox("##skull", &v) && skull && v != before) flipSkull(skull.value());
		active = v;
		nameLine();
		break;
	}
	case ModType::Slider:
	case ModType::HCMSlider:
	{
		ImGui::Dummy(ImVec2(0.f, 0.f));
		nameLine();

		bool hasDefault = mod.defaultValue >= mod.minValue && mod.defaultValue <= mod.maxValue;
		float buttonsW = ImGui::CalcTextSize("SET RESET").x + ImGui::GetStyle().FramePadding.x * 4.f + ImGui::GetStyle().ItemSpacing.x * 2.f;
		ImGui::SetNextItemWidth(std::max(80.f, rowWidth - buttonsW));
		float v = rt.value;
		if (ImGui::SliderFloat("##slider", &v, mod.minValue, mod.maxValue, "%.2f")) rt.value = v;
		bool commit = ImGui::IsItemDeactivatedAfterEdit();
		ImGui::SameLine();
		if (ImGui::Button("SET")) commit = true;
		bool reset = false;
		if (hasDefault)
		{
			ImGui::SameLine();
			if (ImGui::Button("RESET")) { rt.value = mod.defaultValue; commit = true; reset = true; }
		}

		if (commit)
		{
			if (mod.type == ModType::Slider)
			{
				applySlider(mod, rt.value);
			}
			else if (auto setting = hcmDouble(mod.hcmKey))
			{
				setting->GetValueDisplay() = (double)rt.value;
				setting->UpdateValueWithInput();
				addLog(std::format("HCM {} = {}", mod.hcmKey, ModMenuText::formatNumber(rt.value)), LogLine::Kind::Info);
			}
		}
		(void)reset;
		break;
	}
	case ModType::Choice:
	{
		ImGui::Dummy(ImVec2(0.f, 0.f));
		nameLine();

		// only show options this game supports
		std::vector<int> indices;
		for (int i = 0; i < (int)mod.options.size(); i++)
			if (mod.options[i].supports(mGame)) indices.push_back(i);
		if (indices.empty()) break;
		if (std::find(indices.begin(), indices.end(), rt.option) == indices.end()) rt.option = indices.front();

		float buttonsW = ImGui::CalcTextSize("APPLY UNDO").x + ImGui::GetStyle().FramePadding.x * 4.f + ImGui::GetStyle().ItemSpacing.x * 2.f;
		ImGui::SetNextItemWidth(std::max(80.f, rowWidth - buttonsW));
		if (ImGui::BeginCombo("##choice", mod.options[rt.option].label.c_str()))
		{
			for (int i : indices)
			{
				bool sel = i == rt.option;
				if (ImGui::Selectable(mod.options[i].label.c_str(), sel)) rt.option = i;
				if (sel) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button("APPLY")) applyChoice(mod, rt.option);
		ImGui::SameLine();
		if (ImGui::Button("UNDO")) undoChoice(mod);
		if (rt.applied)
			ImGui::TextColored(colGood, "Active: %s", mod.options[rt.option].label.c_str());
		break;
	}
	case ModType::Text:
	{
		ImGui::Dummy(ImVec2(0.f, 0.f));
		nameLine();
		float buttonW = ImGui::CalcTextSize("RUN").x + ImGui::GetStyle().FramePadding.x * 2.f + ImGui::GetStyle().ItemSpacing.x;
		ImGui::SetNextItemWidth(std::max(80.f, rowWidth - buttonW));
		bool enter = ImGui::InputText("##text", &rt.text, ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::Button("RUN") || enter) runText(mod, rt.text);
		break;
	}
	}

	if (!mod.desc.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, colDim);
		ImGui::TextWrapped("%s", mod.desc.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::Separator();
	ImGui::PopID();
}

void ModMenuGUI::renderSkullPanel()
{
	auto skulls = readSkulls();
	if (skulls.empty())
	{
		ImGui::TextColored(colDim, "No skull data for this game yet. Load into a mission first.");
		return;
	}

	int on = 0;
	for (auto& [k, v] : skulls) if (v) on++;
	ImGui::TextColored(colDim, "%d of %d skulls on. Changes apply instantly.", on, (int)skulls.size());
	ImGui::Separator();

	for (auto& [skull, value] : skulls)
	{
		ImGui::PushID((int)skull);
		bool v = value;
		if (ImGui::Checkbox("##sk", &v) && v != value) flipSkull(skull);
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		auto name = std::string(magic_enum::enum_name(skull));
		ImGui::TextColored(value ? colGood : colText, "%s", name.c_str());
		auto it = skullDescriptions().find(skull);
		if (it != skullDescriptions().end())
		{
			ImGui::SameLine();
			ImGui::TextColored(colDim, " %s", it->second);
		}
		ImGui::PopID();
	}
}

void ModMenuGUI::renderLog()
{
	std::deque<LogLine> copy;
	bool scroll = false;
	{
		std::lock_guard<std::mutex> lock(mLogMutex);
		copy = mLog;
		scroll = mScrollLogToBottom;
		mScrollLogToBottom = false;
	}

	ImGui::TextColored(colCyan, "ENGINE BUS");
	ImGui::SameLine();
	ImGui::TextColored(colFaint, "commands sent to MCC and what the engine said back");

	float lineH = ImGui::GetTextLineHeightWithSpacing();
	ImGui::BeginChild("##modmenulog", ImVec2(0.f, lineH * 7.f + 8.f), true);
	for (auto& line : copy)
	{
		ImGui::TextColored(colFaint, "%s", line.time.c_str());
		ImGui::SameLine();
		switch (line.kind)
		{
		case LogLine::Kind::Command: ImGui::TextColored(colAccent, "> %s", line.text.c_str()); break;
		case LogLine::Kind::Output: ImGui::TextColored(colCyan, "< %s", line.text.c_str()); break;
		case LogLine::Kind::Error: ImGui::TextColored(colBad, "! %s", line.text.c_str()); break;
		case LogLine::Kind::Info: ImGui::TextColored(colDim, "- %s", line.text.c_str()); break;
		}
	}
	if (scroll) ImGui::SetScrollHereY(1.f);
	ImGui::EndChild();
}

void ModMenuGUI::renderFooter()
{
	int reapplyCount = 0;
	for (auto& cat : mLibrary.categories)
		for (auto& mod : cat.mods)
			if (mod.reapply && mod.supports(mGame) && isActive(mod)) reapplyCount++;

	auto reapplyLabel = std::format("RE-APPLY ({})", reapplyCount);
	if (ImGui::Button(reapplyLabel.c_str())) reapplyActive("manual re-apply");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sends every active mod again. Use it if a mod wore off.");
	ImGui::SameLine();
	if (ImGui::Button("ALL OFF")) turnEverythingOff();
	ImGui::SameLine();
	if (ImGui::Button("RELOAD LIBRARY")) loadLibrary();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-reads ModMenuLibrary.xml so your edits show up without restarting.");
	ImGui::SameLine();
	if (ImGui::Button("OPEN FOLDER"))
	{
		auto folder = std::filesystem::path(mLibraryPath).parent_path().string();
		ShellExecuteA(nullptr, "open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
	ImGui::SameLine();
	ImGui::Checkbox("Auto re-apply", &mAutoReapply);
	ImGui::SameLine();
	ImGui::Checkbox("Pause while open", &mPauseWhileOpen);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Takes effect the next time you open the menu.");
	ImGui::SameLine();
	ImGui::Checkbox("Log", &mShowLog);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90.f);
	ImGui::SliderFloat("##modmenufont", &mFontSize, 10.f, 28.f, "text %.0f");

	ImGui::TextColored(mLibraryStatusIsError ? colBad : colFaint, "%s", mLibraryStatus.c_str());
}

void ModMenuGUI::render(SimpleMath::Vector2 screenSize)
{
	pushStyle();
	ScopedImFontScaler fontScale(mFontSize);

	ImGui::SetNextWindowPos(ImVec2(screenSize.x * 0.5f, screenSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(std::min(980.f, screenSize.x * 0.8f), std::min(760.f, screenSize.y * 0.85f)), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(520.f, 360.f), ImVec2(screenSize.x, screenSize.y));

	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
	if (ImGui::Begin("Mod Menu##ModMenuGUI", nullptr, flags))
	{
		renderHeader();
		ImGui::Separator();

		float lineH = ImGui::GetTextLineHeightWithSpacing();
		float footerH = ImGui::GetFrameHeightWithSpacing() + lineH + 6.f;
		float logH = mShowLog ? lineH * 8.f + 14.f : 0.f;
		float bodyH = std::max(120.f, ImGui::GetContentRegionAvail().y - footerH - logH);
		float catW = std::max(150.f, ImGui::CalcTextSize("Halo CE Classic  99/99").x + 24.f);

		ImGui::BeginChild("##modmenucats", ImVec2(catW, bodyH), true);
		renderCategoryList();
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("##modmenumods", ImVec2(0.f, bodyH), true);
		renderModList();
		ImGui::EndChild();

		if (mShowLog) renderLog();
		renderFooter();
	}
	ImGui::End();
	popStyle();
}
