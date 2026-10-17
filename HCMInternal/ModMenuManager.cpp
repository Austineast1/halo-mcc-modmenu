#include "pch.h"
#include "ModMenuManager.h"
#include "ModMenuGUI.h"

#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include "ControlServiceContainer.h"
#include "PauseGame.h"
#include "BlockGameInput.h"
#include "FreeMCCCursor.h"
#include "IMakeOrGetCheat.h"
#include "HaloScriptOutputHookEvent.h"
#include "RevertEventHook.h"
#include "GameEngineFunctions.h"
#include "DirPathContainer.h"
#include "GameTickEventHook.h"
#include "GetPlayerDatum.h"

class ModMenuManager::ModMenuManagerImpl
{
private:
	typedef std::optional<std::shared_ptr<SharedRequestToken>> opScRqst;

	GameState mGame;
	std::unique_ptr<ModMenuGUI> gui;

	// injected services
	std::weak_ptr<TokenSharedRequestProvider> hotkeyDisablerWeak;
	std::optional<std::weak_ptr<PauseGame>> pauseGameOptionalWeak;
	std::optional<std::weak_ptr<BlockGameInput>> blockGameInputOptionalWeak;
	std::optional<std::weak_ptr<FreeMCCCursor>> freeCursorOptionalWeak;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<IMessagesGUI> messagesWeak; // HCM's on-screen messages, used to say mods were restored

	// optional engine events
	std::optional<std::shared_ptr<ObservedEvent<HSOutputEvent>>> haloScriptOutputEvent;
	std::optional<std::shared_ptr<ObservedEvent<ActionEvent>>> revertEvent;
	std::unique_ptr<ScopedCallback<ActionEvent>> revertCallback; // only held while a reapply mod is active

	// Death / revert detection that works in every game: the revert hook above only has pointer data for Halo CE,
	// but the game tick counter and the player's unit datum exist for all six. A tick that goes backwards means a
	// checkpoint revert (which is also what dying in solo campaign does) or a restart; a new player unit means a respawn.
	std::weak_ptr<GameTickEventHook> tickSourceWeak;
	std::weak_ptr<GetPlayerDatum> playerDatumWeak;
	std::optional<uint32_t> lastTick;
	std::optional<Datum> lastPlayerUnit;
	int64_t nextResetPollMs = 0;

	// requests held while the menu is open
	struct OpenRequests
	{
		std::shared_ptr<SharedRequestToken> disableHotkeys;
		opScRqst pauseGame;
		opScRqst blockInputs;
		opScRqst freeCursor;
		std::optional<std::shared_ptr<ScopedCallback<HSOutputEvent>>> haloScriptOutputCallback;
	};
	std::optional<OpenRequests> openRequests = std::nullopt;

	// cross-thread signals (revert + mcc state events can fire off the render thread)
	std::atomic<int64_t> reapplyAtMs = 0;
	std::atomic<int> reapplyReason = 0; // 0 revert, 1 level load
	std::atomic_bool closeAsap = false;

	// callbacks (declared last so they're destroyed first)
	std::unique_ptr<ScopedCallback<ActionEvent>> hotkeyCallback;
	std::unique_ptr<ScopedCallback<eventpp::CallbackList<void(const MCCState&)>>> mccStateChangedCallback;
	std::unique_ptr<ScopedCallback<RenderEvent>> renderCallback;

	static int64_t nowMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	void open()
	{
		OpenRequests req;

		if (gui->pauseWhileOpen() && pauseGameOptionalWeak.has_value())
		{
			lockOrThrow(pauseGameOptionalWeak.value(), pauseGame);
			req.pauseGame = pauseGame->makeScopedRequest();
		}

		if (blockGameInputOptionalWeak.has_value())
		{
			lockOrThrow(blockGameInputOptionalWeak.value(), blockGameInput);
			req.blockInputs = blockGameInput->makeScopedRequest();
		}

		if (freeCursorOptionalWeak.has_value())
		{
			lockOrThrow(freeCursorOptionalWeak.value(), freeCursor);
			req.freeCursor = freeCursor->makeScopedRequest();
		}

		lockOrThrow(hotkeyDisablerWeak, hotkeyDisabler);
		req.disableHotkeys = hotkeyDisabler->makeScopedRequest(); // stops HCM hotkeys firing while you type in the search box

		if (haloScriptOutputEvent)
		{
			req.haloScriptOutputCallback = std::make_optional<std::shared_ptr<ScopedCallback<HSOutputEvent>>>(
				haloScriptOutputEvent.value()->subscribe([this](const std::string& s, const HSOutputType& t) { if (gui) gui->onHaloScriptOutput(s, t); }));
		}

		openRequests = std::move(req);
		PLOG_DEBUG << "Mod menu opened for " << mGame.toString();
	}

	void close()
	{
		if (openRequests)
		{
			PLOG_DEBUG << "Mod menu closed for " << mGame.toString();
			openRequests = std::nullopt;
		}
	}

	void onHotkey()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false)
			{
				close();
				return;
			}

			if (openRequests) close();
			else open();
		}
		catch (HCMRuntimeException ex)
		{
			close();
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame)
		{
			closeAsap = true;
			return;
		}
		// our game loaded (or reloaded) a level: give the player a few seconds to spawn, then re-apply
		if (gui) gui->onLevelLoaded();
		reapplyReason = 1;
		reapplyAtMs = nowMs() + 3000;
	}

	void onRevert()
	{
		reapplyReason = 0;
		reapplyAtMs = nowMs() + 800;
	}

	void scheduleReapply(int reason, int64_t delayMs)
	{
		reapplyReason = reason;
		reapplyAtMs = nowMs() + delayMs;
	}

	// polled a few times a second on the render thread while our game is being played
	void pollForResets(bool ourGamePlaying)
	{
		// keep the last values while not playing (loading screens, menus) so a revert or restart that passes
		// through a loading screen is still seen as the tick going backwards
		if (!ourGamePlaying) return;
		auto now = nowMs();
		if (now < nextResetPollMs) return;
		nextResetPollMs = now + 200;
		bool wanted = gui->autoReapply() && gui->hasActiveReapplyMods();

		if (auto tickSource = tickSourceWeak.lock())
		{
			try
			{
				uint32_t tick = tickSource->getCurrentGameTick();
				if (lastTick && tick + 2 < *lastTick && wanted)
				{
					// near zero means the mission restarted or a new level began, so give the player time to spawn
					bool freshStart = tick < 90;
					PLOG_DEBUG << "ModMenu: game tick went back from " << *lastTick << " to " << tick << ", re-applying mods";
					scheduleReapply(freshStart ? 1 : 0, freshStart ? 3000 : 800);
				}
				lastTick = tick;
			}
			catch (HCMRuntimeException) { lastTick.reset(); }
		}

		if (auto playerDatum = playerDatumWeak.lock())
		{
			Datum unit = playerDatum->getPlayerDatum();
			if (!unit.isNull())
			{
				if (lastPlayerUnit && unit != *lastPlayerUnit && wanted && reapplyAtMs.load() == 0)
				{
					PLOG_DEBUG << "ModMenu: player unit changed (" << *lastPlayerUnit << " -> " << unit << "), re-applying mods";
					scheduleReapply(2, 600);
				}
				lastPlayerUnit = unit;
			}
		}
	}

	void updateRevertSubscription()
	{
		if (!revertEvent) return;
		bool want = gui->autoReapply() && gui->hasActiveReapplyMods();
		if (want && !revertCallback)
		{
			revertCallback = revertEvent.value()->subscribe([this]() { onRevert(); });
		}
		else if (!want && revertCallback)
		{
			revertCallback.reset();
		}
	}

	// runs every frame on the render thread
	void onRender(SimpleMath::Vector2 screenSize)
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			bool ourGamePlaying = mccStateHook->isGameCurrentlyPlaying(mGame);

			if (closeAsap.exchange(false) || (openRequests && !ourGamePlaying))
				close();

			pollForResets(ourGamePlaying);

			// pending re-apply after revert / level load
			auto due = reapplyAtMs.load();
			if (due != 0 && nowMs() >= due)
			{
				reapplyAtMs = 0;
				if (ourGamePlaying && gui->autoReapply())
				{
					int reason = reapplyReason.load();
					int count = gui->reapplyActive(reason == 0 ? "a death or checkpoint" : reason == 1 ? "a level load" : "a respawn");
					if (count > 0)
					{
						auto text = std::format("Mod Menu: restored {} mod{} after {}", count, count == 1 ? "" : "s",
							reason == 0 ? "that checkpoint" : reason == 1 ? "loading in" : "respawning");
						gui->notify(text);
						if (auto messages = messagesWeak.lock()) messages->addMessage(text);
					}
				}
			}

			if (!ourGamePlaying) return;

			if (openRequests)
			{
				gui->render(screenSize);

				if (gui->consumeCloseRequest())
					close();

				if (gui->consumeOpenConsoleRequest())
				{
					close();
					lockOrThrow(settingsWeak, settings);
					settings->commandConsoleHotkeyEvent->operator()();
				}
			}

			updateRevertSubscription();
		}
		catch (HCMRuntimeException ex)
		{
			close();
			runtimeExceptions->handleMessage(ex);
		}
	}

public:
	ModMenuManagerImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>())
	{
		auto engine = resolveDependentCheat(GameEngineFunctions); // throws HCMInitException if the engine command interface isn't available
		auto dirPath = dicon.Resolve<DirPathContainer>().lock()->dirPath;
		gui = std::make_unique<ModMenuGUI>(game, engine, settingsWeak, dirPath);

		auto controls = dicon.Resolve<ControlServiceContainer>().lock();
		hotkeyDisablerWeak = controls->hotkeyDisabler;
		pauseGameOptionalWeak = controls->pauseGameService;
		blockGameInputOptionalWeak = controls->blockGameInputService;
		freeCursorOptionalWeak = controls->freeMCCSCursorService;

		try
		{
			auto haloScriptOutputEventProvider = resolveDependentCheat(HaloScriptOutputHookEvent);
			haloScriptOutputEvent = haloScriptOutputEventProvider->getHaloScriptOutputEvent();
		}
		catch (HCMInitException ex)
		{
			PLOG_ERROR << "ModMenu: no HaloScript output for " << game.toString() << ", continuing anyway. Error: " << ex.what();
		}

		try
		{
			tickSourceWeak = resolveDependentCheat(GameTickEventHook);
		}
		catch (HCMInitException ex)
		{
			PLOG_ERROR << "ModMenu: no game tick for " << game.toString() << ", deaths and reverts won't be detected. Error: " << ex.what();
		}

		try
		{
			playerDatumWeak = resolveDependentCheat(GetPlayerDatum);
		}
		catch (HCMInitException ex)
		{
			PLOG_ERROR << "ModMenu: no player datum for " << game.toString() << ", respawns won't be detected. Error: " << ex.what();
		}

		try
		{
			auto revertEventProvider = resolveDependentCheat(RevertEventHook);
			revertEvent = revertEventProvider->getRevertEvent();
		}
		catch (HCMInitException ex)
		{
			PLOG_INFO << "ModMenu: no revert hook for " << game.toString() << " (fine, the game tick is used instead): " << ex.what();
		}

		auto settings = settingsWeak.lock();
		hotkeyCallback = std::make_unique<ScopedCallback<ActionEvent>>(settings->modMenuHotkeyEvent, [this]() { onHotkey(); });
		mccStateChangedCallback = std::make_unique<ScopedCallback<eventpp::CallbackList<void(const MCCState&)>>>(
			dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); });
		renderCallback = std::make_unique<ScopedCallback<RenderEvent>>(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRender(ss); });
	}

	~ModMenuManagerImpl()
	{
		// stop callbacks first so nothing touches the gui while it is torn down
		if (renderCallback) renderCallback->removeCallback();
		if (hotkeyCallback) hotkeyCallback->removeCallback();
		if (mccStateChangedCallback) mccStateChangedCallback->removeCallback();
		revertCallback.reset();
		openRequests = std::nullopt;
	}
};


ModMenuManager::ModMenuManager(GameState gameImpl, IDIContainer& dicon)
{
	pimpl = std::make_unique<ModMenuManagerImpl>(gameImpl, dicon);
}

ModMenuManager::~ModMenuManager() = default;
