#pragma once
#include "pch.h"
#include "GameState.h"
#include "GameEngineFunctions.h"
#include "HaloScriptOutputHookEvent.h"
#include "SettingsStateAndEvents.h"
#include "ModMenuLibrary.h"
#include "SkullEnum.h"
#include "imgui.h"
#include "ModMenuSpawnTypes.h"
#include <deque>

// Renders the in-game Mod Menu window and runs mods.
// Owned by ModMenuManager, which handles opening/closing, input blocking and re-apply timing.
// render() and everything that touches mod state runs on the render thread.
// onHaloScriptOutput can arrive from the game thread, so the log is mutex guarded.

class ModMenuGUI
{
public:
	struct LogLine
	{
		std::string time;
		std::string text;
		enum class Kind { Command, Output, Error, Info } kind;
	};

private:
	struct ModRuntime
	{
		bool on = false;			// toggles
		float value = 0.f;			// sliders
		bool applied = false;		// slider/choice has been applied with a non-default value
		int option = 0;				// choice index
		std::string text;			// text input
	};

	GameState mGame;
	std::shared_ptr<GameEngineFunctions> mEngine;
	std::weak_ptr<SettingsStateAndEvents> mSettingsWeak;
	std::string mLibraryPath;

	ModLibrary mLibrary;
	std::string mLibraryStatus; // shown in footer
	bool mLibraryStatusIsError = false;
	std::unordered_map<std::string, ModRuntime> mState;

	int mSelectedCategory = 0;
	std::string mSearch;
	bool mAutoReapply = true;
	bool mPauseWhileOpen = false;
	bool mShowLog = false;		// engine log drawer, off by default to keep the menu clean
	float mTextScale = 1.f;		// user text size multiplier (Settings)
	float mBasePx = 20.f;		// body text size this frame, derived from screen height * mTextScale
	float mLastFooterH = 0.f;
	ImVec2 mSettingsAnchor{ 0.f, 0.f };
	std::function<std::string(const std::string&)> mCustomAction;

	// spawning (Spawn collection)
	std::function<const std::vector<ModMenuSpawnEntry>*()> mSpawnCatalogue;
	std::function<std::string(const ModMenuSpawnEntry&, bool)> mSpawnAction;
	std::optional<std::pair<bool, bool>> mThirdPersonSaved; // free camera's third-person-rendering and block-input settings before third person
	void setThirdPerson(bool on);
	bool mCanBecome = false;
	std::function<std::string()> mLoadMoreAction;	// true body swap is available in this game (BECOME buttons on characters)
	int mSpawnKind = 0;
	std::string mSpawnFilter;
	bool mCloseRequested = false;
	bool mOpenConsoleRequested = false;

	std::mutex mLogMutex;		// guards mLog and the toast (engine output can arrive off the render thread)
	std::deque<LogLine> mLog;
	const size_t mMaxLog = 60;
	bool mScrollLogToBottom = false;
	int mCommandsSent = 0;

	// alliances (ai_allegiance a b) this menu created; only these are ever removed again
	std::set<std::pair<std::string, std::string>> mAlliancesWeMade;
	bool mPlayerTeamChanged = false; // the menu moved the player to another team (object_set_allegiance)
	std::atomic_bool mForgetAlliances = false; // set on level load (a new level starts with the map's own alliances)

	// one-line confirmation shown in the footer ("Hunter Mode activated")
	std::string mToastText;
	bool mToastIsError = false;
	std::chrono::steady_clock::time_point mToastAt{};
	void toast(const std::string& text, bool isError = false);

	// mod execution
	void sendLines(const std::vector<std::string>& lines, const std::string& value);
	void applyToggle(const ModDef& mod, bool on);
	void applySlider(const ModDef& mod, float value);
	void applyChoice(const ModDef& mod, int optionIndex);
	void undoChoice(const ModDef& mod);
	void runButton(const ModDef& mod);
	void runText(const ModDef& mod, const std::string& text);

	// HCM bridges
	std::shared_ptr<BinarySetting<bool>> hcmBool(const std::string& key);
	std::shared_ptr<BinarySetting<double>> hcmDouble(const std::string& key);
	std::shared_ptr<ActionEvent> hcmAction(const std::string& key);
	bool hcmBoolValue(const ModDef& mod);
	void setHcmBool(const ModDef& mod, bool value);

	// skulls
	std::map<SkullEnum, bool> readSkulls();
	void flipSkull(SkullEnum skull);

	// state helpers
	ModRuntime& runtime(const ModDef& mod);
	bool isActive(const ModDef& mod);
	int activeCount();
	bool matchesSearch(const ModDef& mod) const;
	std::vector<const ModDef*> visibleMods(const ModCategory& cat);

	// rendering
	void pushStyle();
	void popStyle();
	void renderBackdrop();
	void renderHeader();
	void renderCategoryList();
	void renderModList();
	void renderMod(const ModDef& mod);
	void renderSkullPanel();
	void renderSpawnPanel();
	void renderLog(float totalHeight);
	void renderFooter();
	void renderSettingsPopup();

	void loadLibrary();
	void turnEverythingOff();

	// developer self-test: runs every mod for this game one at a time, logging each step first,
	// so if the game crashes the last "TEST" line names the mod that did it
	struct SelfTest
	{
		std::vector<const ModDef*> mods;
		size_t index = 0;
		int phase = 0;
		int64_t nextAtMs = 0;
		int skipped = 0;
	};
	std::optional<SelfTest> mSelfTest;
	std::function<void(const std::string&)> mDevLog;
	void devLog(const std::string& text);
	std::string startSelfTest(const std::string& filter);
	bool sweepSkipReason(const ModDef& mod, std::string& why);
	const ModDef* findMod(const std::string& id);

public:
	ModMenuGUI(GameState game, std::shared_ptr<GameEngineFunctions> engine, std::weak_ptr<SettingsStateAndEvents> settings, std::string dirPath);

	void render(SimpleMath::Vector2 screenSize);
	void onHaloScriptOutput(const std::string& outputString, const HSOutputType& outputType);
	void addLog(const std::string& text, LogLine::Kind kind);

	// Re-sends every active mod marked reapply="true". Called by ModMenuManager after reverts and level loads.
	int reapplyActive(const std::string& reason);
	bool hasActiveReapplyMods();

	// a new level was loaded: alliances made on the previous one no longer exist
	void onLevelLoaded() { mForgetAlliances = true; }

	// actions the manager handles itself (true possession). Returns the message to show.
	void setCustomActionHandler(std::function<std::string(const std::string&)> handler) { mCustomAction = std::move(handler); }

	// spawning: the catalogue of what this level has loaded, and the spawn action (entry, then-become-it)
	void setSpawnHandlers(std::function<const std::vector<ModMenuSpawnEntry>*()> catalogue, std::function<std::string(const ModMenuSpawnEntry&, bool)> action)
	{
		mSpawnCatalogue = std::move(catalogue);
		mSpawnAction = std::move(action);
	}

	void setCanBecome(bool canBecome) { mCanBecome = canBecome; }
	// "load more from this mission" on the Spawn page (Reach designer zones); empty = not offered
	void setLoadMoreHandler(std::function<std::string()> handler) { mLoadMoreAction = std::move(handler); }

	// shows a line in the menu's footer next time it's open
	void notify(const std::string& text) { toast(text); }

	// developer automation (see ModMenuManager: ModMenuDev.txt). Returns what happened.
	std::string devCommand(const std::string& line);
	void setDevLog(std::function<void(const std::string&)> log) { mDevLog = std::move(log); }
	void tickSelfTest(bool gamePlaying);	// every frame
	bool selfTestRunning() const { return mSelfTest.has_value(); }

	bool pauseWhileOpen() const { return mPauseWhileOpen; }
	bool autoReapply() const { return mAutoReapply; }

	// one-shot requests read by the manager
	bool consumeCloseRequest() { bool r = mCloseRequested; mCloseRequested = false; return r; }
	bool consumeOpenConsoleRequest() { bool r = mOpenConsoleRequested; mOpenConsoleRequested = false; return r; }
};
