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
#include "ModMenuPossession.h"
#include "ModMenuLoot.h"
#include "GetObjectPhysics.h"
#include "GetPlayerViewAngle.h"
#include "GetObjectHealth.h"

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
	std::unique_ptr<ModMenuPossession> possession; // true body swap (only where the memory layout is known)
	std::unique_ptr<ModMenuSpawner> spawner;
	std::unique_ptr<ModMenuLoot> loot;			// Borderlands-style weapon rolls (Reach)		// spawn anything the level has loaded

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
	std::weak_ptr<GetObjectPhysics> objectPhysicsWeak;	// where the player is (any game HCM supports)
	std::weak_ptr<GetPlayerViewAngle> viewAngleWeak;		// which way they face
	std::weak_ptr<GetObjectHealth> objectHealthWeak;		// shields and health for the third-person overlay
	int64_t nextResetPollMs = 0;

	// Developer automation: lines in ModMenuDev.txt (next to HCM) are run while this game is being played, then the
	// file is deleted; results are appended to ModMenuDev.log. Lets a script drive the menu: "spawn warthog", "wait 2000",
	// "become elite", "selftest all"... Nothing happens unless that file exists.
	std::string devCommandPath, devLogPath;
	std::deque<std::string> devQueue;
	int64_t nextDevPollMs = 0, devNextLineAtMs = 0;
	bool devDisabled = false;

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
		if (possession) possession->onLevelChanged();
		if (spawner) spawner->onLevelChanged();
		if (loot) loot->onLevelChanged();
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

	// take the player's current unit as the baseline (after a deliberate body swap)
	void forgetPlayerUnit()
	{
		if (auto playerDatum = playerDatumWeak.lock())
		{
			try
			{
				Datum unit = playerDatum->getPlayerDatum();
				if (!unit.isNull()) lastPlayerUnit = unit;
			}
			catch (HCMRuntimeException) {}
		}
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
			Datum unit;
			try { unit = playerDatum->getPlayerDatum(); }
			catch (HCMRuntimeException) { return; } // between lives / loading
			if (!unit.isNull())
			{
				if (lastPlayerUnit && unit != *lastPlayerUnit && wanted && reapplyAtMs.load() == 0 && !(possession && possession->busy()))
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

	// where to put spawned things: the player's position and facing, from HCM's own per-game readers
	std::optional<ModMenuPose> playerPose()
	{
		auto playerDatum = playerDatumWeak.lock();
		auto physics = objectPhysicsWeak.lock();
		if (playerDatum && physics)
		{
			try
			{
				Datum unit = playerDatum->getPlayerDatum();
				if (unit.isNull()) return std::nullopt;
				auto position = physics->getObjectPosition(unit);
				if (!position) return std::nullopt;
				ModMenuPose pose{ *position, 0.f };
				if (auto view = viewAngleWeak.lock())
				{
					try { pose.yaw = view->getPlayerViewAngle().x; }
					catch (HCMRuntimeException) {}
				}
				return pose;
			}
			catch (HCMRuntimeException ex)
			{
				PLOG_DEBUG << "ModMenu: no player pose: " << ex.what();
			}
		}
		return possession ? possession->playerPose() : std::nullopt;
	}

	static std::string lowerCopy(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return s;
	}

	void devLog(const std::string& text)
	{
		if (devLogPath.empty()) return;
		std::ofstream out(devLogPath, std::ios::app);
		if (!out) return;
		auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
		out << std::format("{:%H:%M:%S} UTC [{}] {}\n", now, ModMenuText::shortGameName(mGame), text);
	}

	const ModMenuSpawnEntry* findSpawn(const std::string& query)
	{
		if (!spawner) return nullptr;
		auto& entries = spawner->catalogue();
		auto q = lowerCopy(query);
		for (auto& e : entries) if (lowerCopy(e.name) == q) return &e;
		for (auto& e : entries) if (lowerCopy(e.name).contains(q)) return &e;
		for (auto& e : entries) if (lowerCopy(e.path).contains(q)) return &e;
		return nullptr;
	}

	std::string runDevCommand(const std::string& line)
	{
		auto space = line.find(' ');
		std::string verb = lowerCopy(line.substr(0, space));
		std::string rest = space == std::string::npos ? "" : line.substr(space + 1);
		while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());

		if (verb == "spawn" || verb == "become")
		{
			if (!spawner) return "spawning isn't available in this game";
			if (spawner->catalogue().empty()) return "the spawn list is empty (level still loading?)";
			auto* entry = findSpawn(rest);
			if (!entry) return std::format("nothing called '{}' on this level", rest);
			return std::format("{} ({:08X}): {}", entry->path, entry->tag, spawner->spawn(*entry, verb == "become"));
		}
		if (verb == "catalogue")
		{
			if (!spawner) return "spawning isn't available in this game";
			static const char* kinds[] = { "weapon", "vehicle", "character", "equipment", "prop" };
			auto& entries = spawner->catalogue();
			for (auto& e : entries) devLog(std::format("  {:<9} {:08X} {} = {}", kinds[(int)e.kind], e.tag, e.name, e.path));
			return std::format("{} spawnable tags", entries.size());
		}
		if (verb == "loadmore") return spawner ? spawner->loadMore() : "spawning isn't available in this game";
		if (verb == "possess") return possession ? possession->possessAimed() : "possession isn't available in this game";
		if (verb == "return") return possession ? possession->returnToBody() : "possession isn't available in this game";
		if (verb == "pose")
		{
			auto pose = playerPose();
			if (!pose) return "no pose";
			return std::format("x {:.3f} y {:.3f} z {:.3f} yaw {:.3f}", pose->position.x, pose->position.y, pose->position.z, pose->yaw);
		}
		if (verb == "status")
		{
			std::string out = std::format("{} playing", mGame.toString());
			if (auto tick = tickSourceWeak.lock()) try { out += std::format(", tick {}", tick->getCurrentGameTick()); } catch (HCMRuntimeException) {}
			if (auto pd = playerDatumWeak.lock()) try { out += std::format(", unit {:08X}", (uint32_t)pd->getPlayerDatum()); } catch (HCMRuntimeException) {}
			out += gui->selfTestRunning() ? ", self-test running" : "";
			return out;
		}
		if (verb == "loot")
		{
			if (!loot) return "loot isn't available in this game";
			std::string sub = lowerCopy(rest.substr(0, rest.find(' ')));
			std::string arg = rest.find(' ') == std::string::npos ? "" : rest.substr(rest.find(' ') + 1);
			if (sub == "roll")
			{
				std::optional<uint32_t> seed;
				try { if (!arg.empty()) seed = (uint32_t)std::stoul(arg); } catch (...) {}
				return loot->roll(seed);
			}
			if (sub == "restore") return loot->restore();
			if (sub == "auto") { loot->setAutoReroll(arg != "off"); return arg != "off" ? "new loot every mission: on" : "new loot every mission: off"; }
			if (sub == "list")
			{
				static const char* rarities[] = { "common", "uncommon", "rare", "epic", "LEGENDARY" };
				for (auto& w : loot->weapons())
				{
					std::string stats;
					for (auto& st : w.stats) stats += std::format(" {} {};", st.label, st.value);
					std::string perks;
					for (auto& p : w.perks) perks += " [" + p + "]";
					devLog(std::format("  {:<9} {} {} ({}){}{}", rarities[std::clamp((int)w.rarity, 0, 4)], w.title, w.baseName, w.maker, stats, perks));
				}
				return std::format("{} guns, seed {}", loot->weapons().size(), loot->seed());
			}
			if (sub == "dump")
			{
				auto lines = loot->describeGuns();
				for (auto& l : lines) devLog("  " + l);
				return std::format("{} lines", lines.size());
			}
			return "usage: loot roll [seed] | restore | auto on|off | list | dump";
		}
		return gui->devCommand(line);
	}

	void pollDevCommands(bool ourGamePlaying)
	{
		if (devDisabled || devCommandPath.empty() || !ourGamePlaying) return;
		auto now = nowMs();

		// pick up a new command file about once a second
		if (now >= nextDevPollMs)
		{
			nextDevPollMs = now + 1000;
			std::error_code ec;
			if (std::filesystem::exists(devCommandPath, ec))
			{
				{
					std::ifstream in(devCommandPath);
					std::string line;
					while (std::getline(in, line))
					{
						while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
						while (!line.empty() && line.front() == ' ') line.erase(line.begin());
						if (!line.empty() && line.front() != '#') devQueue.push_back(line);
					}
				}
				std::filesystem::remove(devCommandPath, ec);
				if (ec)
				{
					devDisabled = true; // can't delete it: don't run the same commands every second
					devLog("couldn't delete ModMenuDev.txt, dev commands are off until HCM restarts: " + ec.message());
					devQueue.clear();
				}
			}
		}

		// run queued lines; "wait <ms>" spaces them out so async things (spawns, swaps) can finish
		while (!devQueue.empty() && now >= devNextLineAtMs)
		{
			auto line = devQueue.front();
			devQueue.pop_front();
			if (lowerCopy(line).starts_with("wait"))
			{
				int ms = 1000;
				try { ms = std::stoi(line.substr(4)); } catch (...) {}
				devNextLineAtMs = now + std::clamp(ms, 0, 120000);
				continue;
			}
			std::string result;
			try { result = runDevCommand(line); }
			catch (HCMRuntimeException ex) { result = std::string("error: ") + ex.what(); }
			catch (const std::exception& ex) { result = std::string("error: ") + ex.what(); }
			devLog("> " + line + "  =>  " + result);
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

			if (possession)
			{
				bool swapping = possession->busy();
				if (auto message = possession->update())
				{
					devLog("possession: " + *message);
					gui->notify(*message);
					if (auto messages = messagesWeak.lock()) messages->addMessage("Mod Menu: " + *message);
				}
				// a body swap changes the player's unit on purpose: don't mistake it for a death and re-apply everything
				if (swapping) forgetPlayerUnit();
			}

			pollForResets(ourGamePlaying);

			if (spawner)
			{
				if (auto result = spawner->update())
				{
					devLog(std::format("spawner: {} (object {:08X})", result->message, result->object));
					gui->notify(result->message);
					if (result->becomeIt && result->object != 0xFFFFFFFF)
					{
						// spawned a body to jump into: take it over on the next tick
						auto message = possession ? possession->possessBody(result->object) : std::string("Body swapping isn't available in this game.");
						gui->notify(message);
					}
				}
			}

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

			if (loot)
			{
				if (auto message = loot->update(ourGamePlaying))
				{
					devLog("loot: " + *message);
					gui->notify(*message);
					if (auto messages = messagesWeak.lock()) messages->addMessage("Mod Menu: " + *message);
				}
			}

						pollDevCommands(ourGamePlaying);
			gui->tickSelfTest(ourGamePlaying);

			if (!ourGamePlaying) return;

			// third person hides the game's HUD: draw a crosshair and shield/health bars (not while the menu is open)
			if (!openRequests && gui->thirdPersonActive())
			{
				std::optional<float> shields, health;
				auto playerDatum = playerDatumWeak.lock();
				auto vitals = objectHealthWeak.lock();
				if (playerDatum && vitals)
				{
					try
					{
						Datum unit = playerDatum->getPlayerDatum();
						if (!unit.isNull())
						{
							try { shields = vitals->getObjectShields(unit).value; } catch (HCMRuntimeException) {}
							try { health = vitals->getObjectHealth(unit).value; } catch (HCMRuntimeException) {}
						}
					}
					catch (HCMRuntimeException) {}
				}
				gui->renderThirdPersonOverlay(screenSize, shields, health);
			}

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
		{
			std::string folder = dirPath;
			if (!folder.empty() && folder.back() != '\\' && folder.back() != '/') folder += "\\";
			devCommandPath = folder + "ModMenuDev.txt";
			devLogPath = folder + "ModMenuDev.log";
			gui->setDevLog([this](const std::string& text) { devLog(text); });
		}

		try
		{
			possession = std::make_unique<ModMenuPossession>(game, dicon, engine);
			PLOG_INFO << "ModMenu: true possession available for " << game.toString();
		}
		catch (HCMInitException ex)
		{
			PLOG_INFO << "ModMenu: true possession not available for " << game.toString() << ": " << ex.what();
		}
		try
		{
			try { objectPhysicsWeak = resolveDependentCheat(GetObjectPhysics); }
			catch (HCMInitException ex) { PLOG_INFO << "ModMenu: no object physics for " << game.toString() << ": " << ex.what(); }
			try { objectHealthWeak = resolveDependentCheat(GetObjectHealth); }
			catch (HCMInitException ex) { PLOG_INFO << "ModMenu: no object health for " << game.toString() << ": " << ex.what(); }
			try { viewAngleWeak = resolveDependentCheat(GetPlayerViewAngle); }
			catch (HCMInitException ex) { PLOG_INFO << "ModMenu: no view angle for " << game.toString() << ": " << ex.what(); }
			spawner = std::make_unique<ModMenuSpawner>(game, dicon, [this]() -> std::optional<ModMenuPose> { return playerPose(); });
			PLOG_INFO << "ModMenu: spawning available for " << game.toString();
		}
		catch (HCMInitException ex)
		{
			PLOG_INFO << "ModMenu: spawning not available for " << game.toString() << ": " << ex.what();
		}
		gui->setSpawnHandlers(
			[this]() -> const std::vector<ModMenuSpawnEntry>* { return spawner ? &spawner->catalogue() : nullptr; },
			[this](const ModMenuSpawnEntry& entry, bool becomeIt) -> std::string
			{
				return spawner ? spawner->spawn(entry, becomeIt) : std::string("Spawning isn't available in this game yet.");
			});

		try
		{
			loot = std::make_unique<ModMenuLoot>(game, dicon);
			PLOG_INFO << "ModMenu: loot available for " << game.toString();
			gui->setLootHandlers(
				[this]() -> const std::vector<LootWeapon>* { return loot ? &loot->weapons() : nullptr; },
				[this]() { return loot->roll(); },
				[this]() { return loot->restore(); },
				[this]() { return loot->autoReroll(); },
				[this](bool on) { loot->setAutoReroll(on); });
		}
		catch (HCMInitException ex)
		{
			PLOG_INFO << "ModMenu: loot not available for " << game.toString() << ": " << ex.what();
		}
		gui->setCanBecome(possession != nullptr);
		if (spawner && spawner->canLoadMore())
			gui->setLoadMoreHandler([this]() -> std::string { return spawner ? spawner->loadMore() : std::string("Not available."); });
		gui->setCustomActionHandler([this](const std::string& key) -> std::string
			{
				if (!possession) return "True possession isn't available in this game yet.";
				if (key == "possessAimed") return possession->possessAimed();
				if (key == "possessReturn") return possession->returnToBody();
				return "Unknown action.";
			});

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
