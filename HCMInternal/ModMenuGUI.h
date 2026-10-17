#pragma once
#include "pch.h"
#include "GameState.h"
#include "GameEngineFunctions.h"
#include "HaloScriptOutputHookEvent.h"
#include "SettingsStateAndEvents.h"
#include "ModMenuLibrary.h"
#include "SkullEnum.h"
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
	bool mShowLog = true;
	float mFontSize = 15.f;
	bool mCloseRequested = false;
	bool mOpenConsoleRequested = false;

	std::mutex mLogMutex;
	std::deque<LogLine> mLog;
	const size_t mMaxLog = 60;
	bool mScrollLogToBottom = false;
	int mCommandsSent = 0;

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
	void renderHeader();
	void renderCategoryList();
	void renderModList();
	void renderMod(const ModDef& mod);
	void renderSkullPanel();
	void renderLog();
	void renderFooter();
	void renderGameChips(const ModDef& mod, float rightEdge);
	void renderTag(const std::string& tag);

	void loadLibrary();
	void turnEverythingOff();

public:
	ModMenuGUI(GameState game, std::shared_ptr<GameEngineFunctions> engine, std::weak_ptr<SettingsStateAndEvents> settings, std::string dirPath);

	void render(SimpleMath::Vector2 screenSize);
	void onHaloScriptOutput(const std::string& outputString, const HSOutputType& outputType);
	void addLog(const std::string& text, LogLine::Kind kind);

	// Re-sends every active mod marked reapply="true". Called by ModMenuManager after reverts and level loads.
	int reapplyActive(const std::string& reason);
	bool hasActiveReapplyMods();

	bool pauseWhileOpen() const { return mPauseWhileOpen; }
	bool autoReapply() const { return mAutoReapply; }

	// one-shot requests read by the manager
	bool consumeCloseRequest() { bool r = mCloseRequested; mCloseRequested = false; return r; }
	bool consumeOpenConsoleRequest() { bool r = mOpenConsoleRequested; mOpenConsoleRequested = false; return r; }
};
