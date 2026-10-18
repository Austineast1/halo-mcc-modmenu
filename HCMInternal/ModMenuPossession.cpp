#include "pch.h"
#include "ModMenuPossession.h"
#include "GameEngineFunctions.h"
#include "GameTickEventHook.h"
#include "GetPlayerViewAngle.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"
#include "IMakeOrGetCheat.h"
#include "IGetMCCVersion.h"
#include "Datum.h"
#include "DirPathContainer.h"
#include "ReachGameThread.h"
#include "GetObjectHealth.h"
#include <atomic>
#include <chrono>

namespace
{
	constexpr uint32_t kNone = 0xFFFFFFFF;

	// Memory layout per game and MCC version. Only versions that were inspected live are listed.
	struct Layout
	{
		int64_t unitFieldInPlayer;		// offset of the unit datum inside the player record (= last offset of the playerDatum pointer)
		int64_t unitPlayer;				// unit -> controlling player datum
		int64_t unitActor;				// unit -> AI actor datum
		int64_t objectPosition;			// object -> bounding-sphere centre, always in world space (the "position" field is
										// relative to the parent while seated in a vehicle)
		std::array<int64_t, 2> mappingSlots;	// per-local-user "which unit" slots, relative to the player record
		int64_t mappingPlayerBefore;	// each slot has the player's datum this many bytes before it
		uint8_t bipedType;
		int maxObjects;
		float eyeHeight;				// world units from that centre up to the eyes
	};

	std::optional<Layout> layoutFor(GameState game, std::string_view mccVersion)
	{
		if (game.operator GameState::Value() == GameState::Value::HaloReach && mccVersion == "1.3528.0.0")
			return Layout{ 0x28, 0x1CC, 0x1BC, 0x20, { 0x4A2C, 0x4AE0 }, 0x10, 0, 2048, 0.2f };
		return std::nullopt;
	}

	template <typename T> bool readSafe(uintptr_t address, T& out)
	{
		if (address < 0x10000 || IsBadReadPtr((void*)address, sizeof(T))) return false;
		out = *(T*)address;
		return true;
	}

	template <typename T> bool writeSafe(uintptr_t address, T value)
	{
		if (address < 0x10000 || IsBadWritePtr((void*)address, sizeof(T))) return false;
		*(T*)address = value;
		return true;
	}
}

class ModMenuPossession::Impl
{
private:
	GameState mGame;
	Layout L;
	std::shared_ptr<GameEngineFunctions> mEngine;

	std::shared_ptr<MultilevelPointer> playerUnitPointer;	// "playerDatum": resolves to the player's unit field
	std::shared_ptr<MultilevelPointer> objectMetaDataTable;
	int64_t headerStride = 0x18, headerType = 0x4, headerPointer = 0x10;

	std::weak_ptr<GameTickEventHook> tickWeak;
	std::weak_ptr<GetPlayerViewAngle> viewWeak;
	std::weak_ptr<GetObjectHealth> healthWeak;	// tells an empty body (one you spawned) from a corpse

	std::mutex mMutex;
	struct Request { uint32_t target; bool returning; std::string label; bool silenceAI = true; };
	std::optional<Request> mPending;					// waiting for the game thread
	std::optional<std::string> mResult;				// written by the game thread
	std::optional<std::string> mFollowUpCommand;		// HaloScript to send after the swap
	std::optional<uint32_t> mOriginalBody;			// the body the player started in
	std::atomic_bool mSwapFinished = false;
	std::unique_ptr<ScopedCallback<eventpp::CallbackList<void(uint32_t)>>> mTickCallback;
	int64_t mPendingSinceMs = 0;
	std::optional<uint32_t> mPendingSinceTick;

	// The game's checkpoints save who you're possessing, so after a reload (or a restart of MCC) you can still be in a
	// borrowed body. The body you started in is remembered in this file so "Return to Your Body" still works.
	std::string mStatePath;

	void rememberOriginalBody(std::optional<uint32_t> datum)
	{
		if (mStatePath.empty()) return;
		std::error_code ec;
		if (!datum) { std::filesystem::remove(mStatePath, ec); return; }
		std::ofstream out(mStatePath, std::ios::trunc);
		if (out) out << std::format("{:08X}\n", *datum);
	}

	std::optional<uint32_t> recalledOriginalBody()
	{
		if (mStatePath.empty()) return std::nullopt;
		std::ifstream in(mStatePath);
		std::string text;
		if (!(in >> text)) return std::nullopt;
		try { return (uint32_t)std::stoul(text, nullptr, 16); }
		catch (...) { return std::nullopt; }
	}

	// new session or level reload while in a borrowed body: use the body remembered on disk, if it's still there empty
	void recallOriginalBody(uint32_t current)
	{
		if (mOriginalBody) return;
		auto recalled = recalledOriginalBody();
		if (recalled && *recalled != current && isFreeBody(*recalled)) mOriginalBody = recalled;
	}

	bool isAlive(uint32_t datum)
	{
		auto health = healthWeak.lock();
		if (!health) return false;
		try { return health->getObjectHealth(Datum((uint16_t)(datum & 0xFFFF), (uint16_t)(datum >> 16))).value > 0.f; }
		catch (HCMRuntimeException) { return false; }
	}

	// an empty body nobody drives: a biped with no player and no AI brain
	bool isFreeBody(uint32_t datum)
	{
		uintptr_t meta = 0;
		if (datum == kNone || !objectMetaDataTable->resolve(&meta)) return false;
		uint16_t index = (uint16_t)(datum & 0xFFFF);
		if (index >= L.maxObjects) return false;
		uint8_t type = 0xFF;
		if (!readSafe(meta + headerStride * index + headerType, type) || type != L.bipedType) return false;
		uintptr_t body = objectAddress(datum);
		uint32_t actor = 0, player = 0;
		return body && readSafe(body + L.unitActor, actor) && actor == kNone && readSafe(body + L.unitPlayer, player) && player == kNone;
	}

	std::optional<uint32_t> currentTick()
	{
		auto tick = tickWeak.lock();
		if (!tick) return std::nullopt;
		try { return tick->getCurrentGameTick(); }
		catch (HCMRuntimeException) { return std::nullopt; }
	}

	static int64_t nowMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	void send(const std::string& command)
	{
		try { mEngine->SendCommand(command.c_str()); }
		catch (HCMRuntimeException ex) { PLOG_ERROR << "possession: failed to send " << command << ": " << ex.what(); }
	}

	// ---- memory helpers (valid on any thread, but only consistent on the game thread)

	bool playerRecord(uintptr_t& unitField, uintptr_t& record, uint32_t& playerDatum)
	{
		if (!playerUnitPointer->resolve(&unitField)) return false;
		record = unitField - L.unitFieldInPlayer;
		uint16_t salt = 0;
		if (!readSafe(record, salt) || salt == 0) return false;
		playerDatum = (uint32_t)salt << 16; // player index 0
		return true;
	}

	uintptr_t objectAddress(uint32_t datum)
	{
		if (datum == kNone) return 0;
		uintptr_t meta = 0;
		if (!objectMetaDataTable->resolve(&meta)) return 0;
		uint16_t index = (uint16_t)(datum & 0xFFFF), salt = (uint16_t)(datum >> 16);
		if (index >= L.maxObjects) return 0;
		uintptr_t header = meta + headerStride * index;
		uint16_t headerSalt = 0;
		uintptr_t address = 0;
		if (!readSafe(header, headerSalt) || headerSalt != salt) return 0;
		if (!readSafe(header + headerPointer, address)) return 0;
		return IsBadReadPtr((void*)address, 0x200) ? 0 : address;
	}

	bool readPosition(uintptr_t object, SimpleMath::Vector3& out)
	{
		return readSafe(object + L.objectPosition, out);
	}

	// ---- the swap itself (game thread)

	void onTick(uint32_t)
	{
		if (mSwapFinished) return;
		std::optional<Request> request;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			request = mPending;
		}
		if (!request) return;

		std::string result;
		std::optional<std::string> followUp;
		auto fail = [&](const std::string& why) { result = why; };

		uintptr_t unitField = 0, record = 0;
		uint32_t playerDatum = 0;
		if (!playerRecord(unitField, record, playerDatum)) fail("Couldn't find your player in memory.");
		else
		{
			uint32_t current = *(uint32_t*)unitField;
			uintptr_t currentBody = objectAddress(current);
			uintptr_t targetBody = objectAddress(request->target);
			uint32_t currentPlayer = 0, targetPlayer = 0;

			if (!currentBody) fail("Couldn't find your current body.");
			else if (!targetBody) fail(request->returning ? "Your old body is gone (it may have been destroyed)." : "That creature is gone.");
			else if (!readSafe(currentBody + L.unitPlayer, currentPlayer) || currentPlayer != playerDatum)
				fail("This version of the game is laid out differently, so nothing was changed.");
			else if (!readSafe(targetBody + L.unitPlayer, targetPlayer) || targetPlayer != kNone)
				fail("Another player is already controlling that body.");
			else
			{
				bool mappingOk = true;
				for (auto slot : L.mappingSlots)
				{
					uint32_t slotUnit = 0, slotPlayer = 0;
					if (!readSafe(record + slot, slotUnit) || slotUnit != current) mappingOk = false;
					if (!readSafe(record + slot - L.mappingPlayerBefore, slotPlayer) || slotPlayer != playerDatum) mappingOk = false;
				}
				if (!mappingOk) fail("This version of the game is laid out differently, so nothing was changed.");
				else
				{
					// everything checks out: rewire who controls what
					writeSafe<uint32_t>(currentBody + L.unitPlayer, kNone);
					writeSafe<uint32_t>(targetBody + L.unitPlayer, playerDatum);
					writeSafe<uint32_t>(unitField, request->target);
					for (auto slot : L.mappingSlots) writeSafe<uint32_t>(record + slot, request->target);

					if (request->returning)
					{
						result = "You're back in your own body.";
						mOriginalBody.reset();
					}
					else
					{
						if (!mOriginalBody) mOriginalBody = current;
						result = request->label;
						if (request->silenceAI) followUp = "(ai_braindead (object_get_ai (player_get 0)) true)"; // stop the creature's own AI steering
					}
					PLOG_INFO << "possession: player " << std::hex << playerDatum << " moved from " << current << " to " << request->target;
				}
			}
		}

		std::lock_guard<std::mutex> lock(mMutex);
		mPending.reset();
		mResult = result;
		mFollowUpCommand = followUp;
		mSwapFinished = true;
	}

	void queue(Request request)
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mPending = request;
			mResult.reset();
			mFollowUpCommand.reset();
			mSwapFinished = false;
			mPendingSinceMs = nowMs();
		}
		mPendingSinceTick = currentTick();
		lockOrThrow(tickWeak, tick);
		mTickCallback = tick->getGameTickEvent()->subscribe([this](uint32_t t) { onTick(t); });
	}

public:
	Impl(GameState game, IDIContainer& dicon, std::shared_ptr<GameEngineFunctions> engine)
		: mGame(game), mEngine(engine)
	{
		auto version = dicon.Resolve<IGetMCCVersion>().lock();
		auto layout = layoutFor(game, version ? version->getMCCVersionAsString() : std::string_view());
		if (!layout) throw HCMInitException("True possession has no memory layout for this game/MCC version yet");
		L = *layout;

		if (auto dirs = dicon.Resolve<DirPathContainer>().lock())
		{
			std::string folder = dirs->dirPath;
			if (!folder.empty() && folder.back() != '\\' && folder.back() != '/') folder += "\\";
			mStatePath = folder + "ModMenuPossession.txt";
		}

		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		playerUnitPointer = ptr->getData<std::shared_ptr<MultilevelPointer>>("playerDatum", game);
		objectMetaDataTable = ptr->getData<std::shared_ptr<MultilevelPointer>>("objectMetaDataTable", game);
		headerStride = *ptr->getData<std::shared_ptr<int64_t>>("objectHeaderStride", game);
		headerType = *ptr->getData<std::shared_ptr<int64_t>>("objectTypeOffset", game);
		headerPointer = *ptr->getData<std::shared_ptr<int64_t>>("objectOffsetOffset", game);

		tickWeak = resolveDependentCheat(GameTickEventHook);
		try { healthWeak = resolveDependentCheat(GetObjectHealth); }
		catch (HCMInitException ex) { PLOG_ERROR << "possession: no object health, empty bodies can only be entered with BECOME: " << ex.what(); }
		try { viewWeak = resolveDependentCheat(GetPlayerViewAngle); }
		catch (HCMInitException ex) { PLOG_ERROR << "possession: no view angle, will pick the nearest creature instead: " << ex.what(); }
	}

	~Impl()
	{
		mTickCallback.reset();
	}

	std::string possessAimed()
	{
		if (ReachGameThread::firefight()) return ReachGameThread::kNoBecomeInFirefight;
		if (mPending) return "Still taking over... close the menu if the game is paused.";

		uintptr_t unitField = 0, record = 0;
		uint32_t playerDatum = 0;
		if (!playerRecord(unitField, record, playerDatum)) return "Load into a mission first.";
		uint32_t current = *(uint32_t*)unitField;
		recallOriginalBody(current);
		uintptr_t currentBody = objectAddress(current);
		SimpleMath::Vector3 eye;
		if (!currentBody || !readPosition(currentBody, eye)) return "Couldn't find your body. Are you alive?";
		eye.z += L.eyeHeight;

		// aim direction
		std::optional<SimpleMath::Vector3> aim;
		if (auto view = viewWeak.lock())
		{
			try
			{
				auto angles = view->getPlayerViewAngle(); // yaw, pitch (radians)
				aim = SimpleMath::Vector3(std::cos(angles.y) * std::cos(angles.x), std::cos(angles.y) * std::sin(angles.x), std::sin(angles.y));
			}
			catch (HCMRuntimeException) {}
		}

		// score every AI-driven biped: small angle to the crosshair wins, distance breaks ties
		uintptr_t meta = 0;
		if (!objectMetaDataTable->resolve(&meta)) return "Couldn't read the object list.";
		uint32_t best = kNone, nearest = kNone;
		float bestScore = 1e9f, bestDistance = 0.f, nearestDistance = 1e9f;
		for (int i = 0; i < L.maxObjects; i++)
		{
			uintptr_t header = meta + headerStride * i;
			uint16_t salt = 0;
			uint8_t type = 0xFF;
			uintptr_t object = 0;
			if (!readSafe(header, salt) || salt == 0) continue;
			if (!readSafe(header + headerType, type) || type != L.bipedType) continue;
			if (!readSafe(header + headerPointer, object) || IsBadReadPtr((void*)object, 0x200)) continue;
			uint32_t datum = ((uint32_t)salt << 16) | (uint32_t)i;
			if (datum == current) continue;

			uint32_t actor = kNone, player = kNone;
			if (!readSafe(object + L.unitPlayer, player) || player != kNone) continue;	// someone's driving it
			if (!readSafe(object + L.unitActor, actor)) continue;
			if (actor == kNone && !isAlive(datum)) continue;	// no AI brain and no health: a corpse
			SimpleMath::Vector3 position;
			if (!readPosition(object, position)) continue;

			SimpleMath::Vector3 toTarget = position - eye;
			float distance = toTarget.Length();
			if (distance < 0.01f || distance > 40.f) continue; // 40 world units is about 120 m
			if (distance < nearestDistance) { nearestDistance = distance; nearest = datum; }
			float score;
			if (aim)
			{
				float along = toTarget.Dot(*aim);
				if (along <= 0.f) continue;
				float angle = std::atan2((toTarget - *aim * along).Length(), along);
				if (angle > 0.30f) continue; // ~17 degrees
				score = angle + distance * 0.004f;
			}
			else
			{
				score = distance;
			}
			if (score < bestScore) { bestScore = score; best = datum; bestDistance = distance; }
		}
		// nothing in the sights: fall back to whoever is standing right next to you (old-school bump possession)
		if (best == kNone && nearestDistance < 2.75f) { best = nearest; bestDistance = nearestDistance; }
		if (best == kNone)
			return aim ? "Aim right at a creature (Hunter, Elite, Grunt, Marine...) or stand next to one, and try again." : "No creature close enough to take over.";

		// aimed at the body you started in: that's just going back
		if (mOriginalBody && best == *mOriginalBody) return returnToBody();

		// leaving a body we'd already taken over: give it its brain back first
		if (mOriginalBody && current != *mOriginalBody)
			send("(ai_braindead (object_get_ai (player_get 0)) false)");

		uint32_t bestActor = kNone;
		readSafe(objectAddress(best) + L.unitActor, bestActor);
		try
		{
			queue(Request{ best, false, std::format("You took over that creature ({:.0f} m away). \"Return to Your Body\" takes you back.", bestDistance * 3.048f), bestActor != kNone });
		}
		catch (HCMRuntimeException ex)
		{
			return std::format("Couldn't start the takeover: {}", ex.what());
		}
		return "Taking over...";
	}

	std::string returnToBody()
	{
		if (mPending) return "Still working on the last swap...";
		uintptr_t unitField = 0, record = 0;
		uint32_t playerDatum = 0;
		if (!playerRecord(unitField, record, playerDatum)) return "Load into a mission first.";
		uint32_t current = *(uint32_t*)unitField;
		recallOriginalBody(current);
		if (!mOriginalBody || current == *mOriginalBody) { mOriginalBody.reset(); rememberOriginalBody(std::nullopt); return "You're already in your own body."; }
		if (!objectAddress(*mOriginalBody)) { mOriginalBody.reset(); rememberOriginalBody(std::nullopt); return "Your old body is gone (it was destroyed). Restart the mission to get it back."; }

		send("(ai_braindead (object_get_ai (player_get 0)) false)"); // the creature gets its brain back
		try { queue(Request{ *mOriginalBody, true, "" }); }
		catch (HCMRuntimeException ex) { return std::format("Couldn't go back: {}", ex.what()); }
		return "Returning to your body...";
	}

	std::string possessBody(uint32_t target)
	{
		if (ReachGameThread::firefight()) return ReachGameThread::kNoBecomeInFirefight;
		if (mPending) return "Still working on the last swap...";
		uintptr_t unitField = 0, record = 0;
		uint32_t playerDatum = 0;
		if (!playerRecord(unitField, record, playerDatum)) return "Load into a mission first.";
		uint32_t current = *(uint32_t*)unitField;
		recallOriginalBody(current);
		uintptr_t body = objectAddress(target);
		if (!body) return "That body is gone.";
		uint32_t actor = kNone;
		readSafe(body + L.unitActor, actor);
		if (mOriginalBody && current != *mOriginalBody)
			send("(ai_braindead (object_get_ai (player_get 0)) false)");
		try { queue(Request{ target, false, "You're in the new body. \"Return to Your Body\" takes you back.", actor != kNone }); }
		catch (HCMRuntimeException ex) { return std::format("Couldn't jump in: {}", ex.what()); }
		return "Jumping in...";
	}

	std::optional<ModMenuPose> playerPose()
	{
		uintptr_t unitField = 0, record = 0;
		uint32_t playerDatum = 0;
		if (!playerRecord(unitField, record, playerDatum)) return std::nullopt;
		uintptr_t body = objectAddress(*(uint32_t*)unitField);
		ModMenuPose pose{};
		if (!body || !readPosition(body, pose.position)) return std::nullopt;
		pose.yaw = 0.f;
		if (auto view = viewWeak.lock())
		{
			try { pose.yaw = view->getPlayerViewAngle().x; }
			catch (HCMRuntimeException) {}
		}
		return pose;
	}

	std::optional<std::string> update()
	{
		if (!mSwapFinished)
		{
			// Still queued. While the game is paused its tick stands still and we keep waiting; if the game is running
			// and the swap still hasn't happened, the tick hook isn't working: give up rather than swap minutes later.
			if (!mPending) return std::nullopt;
			auto tick = currentTick();
			bool stalled = (tick && mPendingSinceTick) ? (*tick > *mPendingSinceTick + 90) : (nowMs() - mPendingSinceMs > 15000);
			if (!stalled) return std::nullopt;
			{
				std::lock_guard<std::mutex> lock(mMutex);
				mPending.reset();
			}
			mTickCallback.reset();
			return std::string("The swap didn't happen (the game didn't run it). Try again.");
		}
		mTickCallback.reset();
		mSwapFinished = false;
		std::optional<std::string> result, followUp;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			result = mResult;
			followUp = mFollowUpCommand;
			mResult.reset();
			mFollowUpCommand.reset();
		}
		if (followUp) send(*followUp);
		rememberOriginalBody(mOriginalBody); // keep the file in step: written after leaving your body, removed on return
		return result;
	}

	void onLevelChanged()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mOriginalBody.reset();
		mPending.reset();
	}

	bool busy() { return mPending.has_value() || mSwapFinished; }
};

ModMenuPossession::ModMenuPossession(GameState game, IDIContainer& dicon, std::shared_ptr<GameEngineFunctions> engine)
	: pimpl(std::make_unique<Impl>(game, dicon, engine)) {}
ModMenuPossession::~ModMenuPossession() = default;
std::string ModMenuPossession::possessAimed() { return pimpl->possessAimed(); }
std::string ModMenuPossession::returnToBody() { return pimpl->returnToBody(); }
std::string ModMenuPossession::possessBody(uint32_t unitDatum) { return pimpl->possessBody(unitDatum); }
std::optional<ModMenuPose> ModMenuPossession::playerPose() { return pimpl->playerPose(); }
std::optional<std::string> ModMenuPossession::update() { return pimpl->update(); }
void ModMenuPossession::onLevelChanged() { pimpl->onLevelChanged(); }
bool ModMenuPossession::busy() { return pimpl->busy(); }
