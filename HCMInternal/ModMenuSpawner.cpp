#include "pch.h"
#include "ModMenuSpawner.h"
#include "GameTickEventHook.h"
#include "TagTableRange.h"
#include "GetTagName.h"
#include "IMakeOrGetCheat.h"
#include "IGetMCCVersion.h"
#include "ModuleCache.h"
#include "MagicString.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <map>

namespace
{
	constexpr uint32_t kNone = 0xFFFFFFFF;
	constexpr uint32_t kCrashed = 0xFFFFFFFE;

	using TagAvailableFn = bool(__fastcall*)(uint32_t tag);
	using PlacementNewFn = void(__fastcall*)(void* placement, uint32_t definition, uint32_t owner, void* damageOwner);
	using ObjectNewFn = uint32_t(__fastcall*)(void* placement);

	struct Layout
	{
		const wchar_t* module;
		uintptr_t placementNew, objectNew;
		std::vector<uint8_t> placementNewBytes, objectNewBytes;
		size_t placementSize;
		size_t placementPosition, placementForward;
		// the engine's own "is this tag loaded in the current zone set" check (object_new refuses tags that aren't)
		uintptr_t tagAvailable;
		std::vector<uint8_t> tagAvailableBytes;
	};

	// object_placement_data_new and object_new were found by following each game's HaloScript object_create
	// (object_create -> create-by-name -> placement-from-scenario -> placement_new, then object_new). Halo 3 and ODST
	// use a 0x180-byte placement block with the same field offsets as Reach.
	std::optional<Layout> layoutFor(GameState game, std::string_view mccVersion)
	{
		if (mccVersion != "1.3528.0.0") return std::nullopt;
		static const std::vector<uint8_t> h3PlacementNew = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20,
			0x41, 0x8B, 0xE8, 0x8B, 0xDA, 0x33, 0xD2, 0x41, 0xB8, 0x80, 0x01, 0x00, 0x00, 0x49, 0x8B, 0xF1, 0x48, 0x8B, 0xF9, 0xE8 };
		if (game.operator GameState::Value() == GameState::Value::Halo3)
		{
			return Layout{ L"halo3.dll", 0x341508, 0x341AE8, h3PlacementNew,
				{ 0x48, 0x8B, 0xC4, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x78, 0x83,
				  0xCD, 0xFF, 0x45, 0x33, 0xE4, 0xF6, 0x41, 0x18, 0x10, 0x48, 0x8B, 0xF1, 0x8B, 0xFD, 0x44, 0x88, 0x60, 0x10, 0x75, 0x09 },
				0x180, 0x1C, 0x28,
				0x12C9EC, { 0x83, 0xF9, 0xFF, 0x74, 0x25, 0x48, 0x8B, 0x05, 0x30, 0x76, 0x7C, 0x00 } };
		}
		if (game.operator GameState::Value() == GameState::Value::Halo3ODST)
		{
			return Layout{ L"halo3odst.dll", 0x37C954, 0x37CF34, h3PlacementNew,
				{ 0x48, 0x8B, 0xC4, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x88, 0x00,
				  0x00, 0x00, 0x41, 0x83, 0xCE, 0xFF, 0x33, 0xED, 0xF6, 0x41, 0x18, 0x10, 0x48, 0x8B, 0xF1, 0x41, 0x8B, 0xFE, 0x40, 0x88 },
				0x180, 0x1C, 0x28,
				0x14D4C8, { 0x83, 0xF9, 0xFF, 0x74, 0x25, 0x48, 0x8B, 0x05, 0x74, 0x53, 0x7E, 0x00 } };
		}
		if (game.operator GameState::Value() == GameState::Value::HaloReach)
		{
			return Layout{ L"haloreach.dll", 0x49CC24, 0x46D6E0,
				{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20,
				  0x41, 0x8B, 0xE8, 0x8B, 0xDA, 0x33, 0xD2, 0x41, 0xB8, 0xA8, 0x01, 0x00, 0x00, 0x49, 0x8B, 0xF1, 0x48, 0x8B, 0xF9, 0xE8 },
				{ 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00,
				  0x8B, 0x41, 0x18, 0x49, 0x83, 0xCC, 0xFF, 0x45, 0x33, 0xC0, 0xC1, 0xE8, 0x04, 0x44, 0x88, 0x44, 0x24, 0x50, 0x4C, 0x8B },
				0x1A8, 0x1C, 0x28,
				0x8F620, { 0x83, 0xF9, 0xFF, 0x74, 0x22, 0x48, 0x8B, 0x05, 0x24, 0x3F, 0xB0, 0x00 } };
		}
		return std::nullopt;
	}

	// Kept free of C++ objects so it can use SEH: if the engine faults we report it instead of taking MCC down.
	uint32_t callEngineSpawn(PlacementNewFn placementNew, ObjectNewFn objectNew, uint8_t* placement, size_t positionOffset, size_t forwardOffset,
		uint32_t definition, const float* position, const float* forward)
	{
		__try
		{
			placementNew(placement, definition, kNone, nullptr);
			memcpy(placement + positionOffset, position, sizeof(float) * 3);
			memcpy(placement + forwardOffset, forward, sizeof(float) * 3);
			return objectNew(placement);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return kCrashed;
		}
	}

	bool callTagAvailable(TagAvailableFn fn, uint32_t tag)
	{
		__try { return fn(tag); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	std::string titleCase(std::string s)
	{
		bool start = true;
		for (auto& c : s)
		{
			if (c == '_' || c == '-') { c = ' '; start = true; continue; }
			if (start && std::isalpha((unsigned char)c)) c = (char)std::toupper((unsigned char)c);
			start = c == ' ';
		}
		return s;
	}

	// "objects\weapons\rifle\assault_rifle\assault_rifle" -> "Assault Rifle"
	// "objects\vehicles\human\warthog\turrets\gauss\weapon\gauss_turret" -> "Gauss Turret"
	std::string prettyName(std::string_view path)
	{
		auto slash = path.find_last_of("\\/");
		std::string last(slash == std::string_view::npos ? path : path.substr(slash + 1));
		std::string name = titleCase(last);
		// "Elite Ai" -> "Elite (AI)", "Spartans" -> "Spartan", "Dmr" -> "DMR"
		if (name.ends_with(" Ai")) name = name.substr(0, name.size() - 3) + " (AI)";
		if (name.starts_with("Spartans")) name = "Spartan" + name.substr(8);
		static const std::pair<const char*, const char*> fixes[] = {
			{ "Dmr", "DMR" }, { "Smg", "SMG" }, { "Br", "BR" }, { "Odst", "ODST" }, { "Reach Moa", "Moa" }, { "Unsc", "UNSC" },
		};
		for (auto& [from, to] : fixes)
		{
			std::string f = from;
			if (name == f) { name = to; break; }
			if (name.starts_with(f + " ")) { name = std::string(to) + name.substr(f.size()); break; }
			if (name.ends_with(" " + f)) { name = name.substr(0, name.size() - f.size()) + to; break; }
		}
		return name;
	}

	std::optional<SpawnKind> kindOf(int32_t groupMagic)
	{
		static const std::pair<const char*, SpawnKind> groups[] = {
			{ "weap", SpawnKind::Weapon }, { "vehi", SpawnKind::Vehicle }, { "bipd", SpawnKind::Character },
			{ "eqip", SpawnKind::Equipment }, { "bloc", SpawnKind::Prop },
		};
		for (auto& [magic, kind] : groups)
			if (MagicString(std::string(magic)).getNum() == groupMagic) return kind;
		return std::nullopt;
	}

	// Leave out what isn't fun or useful to spawn on its own: first-person arms, test content, debris pieces,
	// cinematic heads, vehicle parts and weapons that only exist bolted to a vehicle.
	bool worthListing(SpawnKind kind, const std::string& p) // p is the lower-case tag path
	{
		for (auto* junk : { "fp_", "first_person", "\\fp\\", "test", "garbage", "_broken", "\\heads\\", "\\attachments\\", "\\projectiles\\", "\\null\\" })
			if (p.contains(junk)) return false;
		bool onAVehicle = p.starts_with("objects\\vehicles\\");
		bool standaloneTurret = p.contains("vehicles\\human\\turrets\\") || p.contains("vehicles\\covenant\\turrets\\");
		switch (kind)
		{
		case SpawnKind::Weapon:
			if (p.ends_with("unarmed") || p.contains("horn") || p.contains("sheath")) return false;
			return !onAVehicle || standaloneTurret || p.substr(p.find_last_of('\\') + 1).contains("turret");
		case SpawnKind::Vehicle:
			return !p.contains("\\turrets\\") || standaloneTurret;
		default:
			return true;
		}
	}

	// how far in front of the player each kind appears (world units; 1 wu is about 3 m), and how high
	float spawnDistance(SpawnKind k) { switch (k) { case SpawnKind::Vehicle: return 1.8f; case SpawnKind::Character: return 0.8f; case SpawnKind::Prop: return 0.7f; default: return 0.45f; } }
	float spawnHeight(SpawnKind k) { switch (k) { case SpawnKind::Vehicle: return 0.35f; case SpawnKind::Character: return 0.15f; default: return 0.25f; } }
}

class ModMenuSpawner::Impl
{
private:
	GameState mGame;
	Layout L;
	std::function<std::optional<ModMenuPose>()> mPose;
	std::weak_ptr<GameTickEventHook> tickWeak;
	std::weak_ptr<TagTableRange> tagTableWeak;
	std::weak_ptr<GetTagName> tagNameWeak;

	std::vector<ModMenuSpawnEntry> mCatalogue;	// render thread only
	std::atomic_bool mCatalogueStale = false;	// set from any thread when the level changes
	bool mCatalogueBuilt = false;
	int64_t mCatalogueBuiltAtMs = 0;			// rebuilt every so often: Reach and Halo 3 load new zone sets mid-mission
	int64_t mCatalogueRetryAtMs = 0;

	struct Request { ModMenuSpawnEntry entry; bool becomeIt; float position[3]; float forward[3]; };
	static constexpr size_t kMaxQueued = 12;
	std::mutex mMutex;
	std::deque<Request> mPending;	// waiting for the game thread (one is spawned per tick)
	std::deque<Result> mResults;		// finished, waiting to be reported on the render thread
	// render thread only
	std::unique_ptr<ScopedCallback<eventpp::CallbackList<void(uint32_t)>>> mTickCallback;
	std::optional<uint32_t> mWaitingSinceTick;
	int64_t mWaitingSinceMs = 0;

	static int64_t nowMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	// the engine's own loaded-in-this-zone-set check, if its bytes are what we expect
	std::optional<TagAvailableFn> tagAvailableCheck()
	{
		auto module = ModuleCache::getModuleHandle(L.module);
		if (!module) return std::nullopt;
		auto address = (uintptr_t)module.value() + L.tagAvailable;
		if (IsBadReadPtr((void*)address, L.tagAvailableBytes.size()) || memcmp((void*)address, L.tagAvailableBytes.data(), L.tagAvailableBytes.size()) != 0)
			return std::nullopt;
		return (TagAvailableFn)address;
	}

	bool functionsMatch(uintptr_t base)
	{
		auto matches = [](uintptr_t address, const std::vector<uint8_t>& bytes)
			{
				if (IsBadReadPtr((void*)address, bytes.size())) return false;
				return memcmp((void*)address, bytes.data(), bytes.size()) == 0;
			};
		return matches(base + L.placementNew, L.placementNewBytes) && matches(base + L.objectNew, L.objectNewBytes);
	}

	// the level may have changed since the list was built: only spawn a tag that is still there, with the same type
	bool tagStillLoaded(const ModMenuSpawnEntry& entry)
	{
		auto tagTable = tagTableWeak.lock();
		if (!tagTable) return false;
		auto tag = tagTable->getTagByDatum(Datum((uint16_t)(entry.tag & 0xFFFF), (uint16_t)(entry.tag >> 16)));
		if (!tag) return false;
		auto kind = kindOf(tag.value().tagType.getNum());
		if (!kind || *kind != entry.kind || (uint32_t)tag.value().tagDatum != entry.tag) return false;
		auto available = tagAvailableCheck();
		return !available || callTagAvailable(*available, entry.tag);
	}

	// game thread
	void onTick(uint32_t)
	{
		std::optional<Request> request;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			if (mPending.empty()) return;
			request = mPending.front();
			mPending.pop_front();
		}

		Result result{ "", kNone, request->becomeIt };
		auto module = ModuleCache::getModuleHandle(L.module);
		if (!module) result.message = "The game isn't loaded.";
		else if (!functionsMatch((uintptr_t)module.value())) result.message = "This version of the game doesn't match, so nothing was spawned.";
		else if (!tagStillLoaded(request->entry)) result.message = std::format("{} isn't loaded on this level any more. Reopen the Spawn list.", request->entry.name);
		else
		{
			alignas(16) uint8_t placement[0x400] = {};
			uint32_t object = callEngineSpawn((PlacementNewFn)((uintptr_t)module.value() + L.placementNew), (ObjectNewFn)((uintptr_t)module.value() + L.objectNew),
				placement, L.placementPosition, L.placementForward, request->entry.tag, request->position, request->forward);
			if (object == kCrashed) result.message = std::format("The game refused to spawn {} here.", request->entry.name);
			else if (object == kNone) result.message = std::format("{} couldn't be spawned here (the level may not have all of its parts loaded).", request->entry.name);
			else
			{
				result.object = object;
				result.message = std::format("Spawned {}.", request->entry.name);
				PLOG_INFO << "spawner: spawned " << request->entry.path << " as object " << std::hex << object;
			}
		}

		std::lock_guard<std::mutex> lock(mMutex);
		mResults.push_back(result);
	}

	std::optional<uint32_t> currentTick()
	{
		auto tick = tickWeak.lock();
		if (!tick) return std::nullopt;
		try { return tick->getCurrentGameTick(); }
		catch (HCMRuntimeException) { return std::nullopt; }
	}

	void buildCatalogue()
	{
		mCatalogue.clear();
		auto tagTable = tagTableWeak.lock();
		auto tagName = tagNameWeak.lock();
		if (!tagTable || !tagName) return;
		auto tags = tagTable->getTagTableRange();
		if (!tags)
		{
			PLOG_ERROR << "spawner: couldn't read the tag table: " << tags.error().what();
			return;
		}
		std::map<std::string, int> nameCounts;
		auto available = tagAvailableCheck();
		int notLoaded = 0;
		for (auto& tag : tags.value())
		{
			auto kind = kindOf(tag.tagType.getNum());
			if (!kind) continue;
			std::string path;
			try
			{
				const char* name = tagName->getTagName(tag.tagDatum);
				if (name) path = name;
			}
			catch (HCMRuntimeException) {}
			if (path.empty()) continue;
			std::string lowered = path;
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			if (!worthListing(*kind, lowered)) continue;
			// the level references it but this part of the level doesn't have it loaded: the game would refuse it
			if (available && !callTagAvailable(*available, (uint32_t)tag.tagDatum)) { notLoaded++; continue; }

			ModMenuSpawnEntry entry{ (uint32_t)tag.tagDatum, *kind, prettyName(path), path };
			nameCounts[entry.name]++;
			mCatalogue.push_back(std::move(entry));
		}
		// two tags with the same short name: add the nearest folder that tells them apart
		for (auto& e : mCatalogue)
		{
			if (nameCounts[e.name] < 2) continue;
			std::string_view p = e.path;
			auto end = p.find_last_of("\\/");
			while (end != std::string_view::npos && end > 0)
			{
				auto start = p.substr(0, end).find_last_of("\\/");
				size_t from = start == std::string_view::npos ? 0 : start + 1;
				auto folder = titleCase(std::string(p.substr(from, end - from)));
				if (folder != e.name && folder != "Weapon" && folder != "Weapons")
				{
					e.name += " (" + folder + ")";
					break;
				}
				if (start == std::string_view::npos) break;
				end = start;
			}
		}
		std::sort(mCatalogue.begin(), mCatalogue.end(), [](const auto& a, const auto& b)
			{ return a.kind != b.kind ? a.kind < b.kind : a.name < b.name; });
		PLOG_INFO << "spawner: catalogue has " << mCatalogue.size() << " entries (" << notLoaded << " more aren't loaded in this zone set)"
			<< (available ? "" : "; the zone-set check didn't match, so nothing was filtered");
	}

public:
	Impl(GameState game, IDIContainer& dicon, std::function<std::optional<ModMenuPose>()> poseProvider)
		: mGame(game), mPose(std::move(poseProvider))
	{
		auto version = dicon.Resolve<IGetMCCVersion>().lock();
		auto layout = layoutFor(game, version ? version->getMCCVersionAsString() : std::string_view());
		if (!layout) throw HCMInitException("Spawning has no engine layout for this game/MCC version yet");
		L = *layout;
		tickWeak = resolveDependentCheat(GameTickEventHook);
		tagTableWeak = resolveDependentCheat(TagTableRange);
		tagNameWeak = resolveDependentCheat(GetTagName);
	}

	~Impl() { mTickCallback.reset(); }

	const std::vector<ModMenuSpawnEntry>& catalogue()
	{
		if (mCatalogueStale.exchange(false))
		{
			mCatalogue.clear();
			mCatalogueBuilt = false;
			mCatalogueRetryAtMs = nowMs() + 1500; // let the new level finish loading
		}
		if (mCatalogueBuilt && nowMs() - mCatalogueBuiltAtMs > 20000) mCatalogueBuilt = false;
		if (!mCatalogueBuilt && nowMs() >= mCatalogueRetryAtMs)
		{
			try { buildCatalogue(); }
			catch (HCMRuntimeException ex) { PLOG_ERROR << "spawner: " << ex.what(); mCatalogue.clear(); }
			mCatalogueBuilt = !mCatalogue.empty();
			mCatalogueBuiltAtMs = nowMs();
			if (!mCatalogueBuilt) mCatalogueRetryAtMs = nowMs() + 3000; // level still loading, try again shortly
		}
		return mCatalogue;
	}

	std::string spawn(const ModMenuSpawnEntry& entry, bool becomeIt)
	{
		auto pose = mPose ? mPose() : std::nullopt;
		if (!pose) return "Load into a mission first.";

		size_t queued;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			queued = mPending.size();
		}
		if (queued >= kMaxQueued) return "Hold on, still spawning the last few...";

		float distance = spawnDistance(entry.kind), height = spawnHeight(entry.kind);
		Request request{ entry, becomeIt, {}, {} };
		float fx = std::cos(pose->yaw), fy = std::sin(pose->yaw);
		// several spawns queued at once: fan them out sideways so they don't land inside each other
		float side = queued == 0 ? 0.f : ((queued % 2) ? 1.f : -1.f) * (float)((queued + 1) / 2) * (entry.kind == SpawnKind::Vehicle ? 1.2f : 0.35f);
		request.position[0] = pose->position.x + fx * distance - fy * side;
		request.position[1] = pose->position.y + fy * distance + fx * side;
		request.position[2] = pose->position.z + height;
		request.forward[0] = fx;
		request.forward[1] = fy;
		request.forward[2] = 0.f;
		if (entry.kind == SpawnKind::Character)
		{
			// face the player so you can see who you spawned
			request.forward[0] = -fx;
			request.forward[1] = -fy;
		}

		try
		{
			if (!mTickCallback)
			{
				lockOrThrow(tickWeak, tick);
				mTickCallback = tick->getGameTickEvent()->subscribe([this](uint32_t t) { onTick(t); });
			}
		}
		catch (HCMRuntimeException ex)
		{
			return std::format("Couldn't spawn: {}", ex.what());
		}
		{
			std::lock_guard<std::mutex> lock(mMutex);
			if (mPending.empty()) { mWaitingSinceTick = currentTick(); mWaitingSinceMs = nowMs(); }
			mPending.push_back(request);
		}
		return std::format("Spawning {}...", entry.name);
	}

	std::optional<Result> update()
	{
		bool idle = false, stalled = false;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			if (!mResults.empty())
			{
				auto result = mResults.front();
				mResults.pop_front();
				// progress was made: restart the stall clock for whatever is still queued
				mWaitingSinceTick = currentTick();
				mWaitingSinceMs = nowMs();
				return result;
			}
			if (mPending.empty()) idle = true;
			else
			{
				// Queued but nothing spawned. If the game is paused the tick stands still and we keep waiting; if the game
				// is running and our tick hook still never ran, give up instead of spawning something minutes later.
				auto tick = currentTick();
				stalled = (tick && mWaitingSinceTick) ? (*tick > *mWaitingSinceTick + 90) : (nowMs() - mWaitingSinceMs > 15000);
				if (stalled) mPending.clear();
			}
		}
		// unsubscribe outside the lock (the game thread may be waiting on it inside onTick)
		if (idle || stalled) mTickCallback.reset();
		if (stalled) return Result{ "Nothing spawned: the game didn't run the request. Try again.", kNone, false };
		return std::nullopt;
	}

	// any thread
	void onLevelChanged()
	{
		mCatalogueStale = true;
		std::lock_guard<std::mutex> lock(mMutex);
		mPending.clear();
	}
};

ModMenuSpawner::ModMenuSpawner(GameState game, IDIContainer& dicon, std::function<std::optional<ModMenuPose>()> poseProvider)
	: pimpl(std::make_unique<Impl>(game, dicon, std::move(poseProvider))) {}
ModMenuSpawner::~ModMenuSpawner() = default;
const std::vector<ModMenuSpawnEntry>& ModMenuSpawner::catalogue() { return pimpl->catalogue(); }
std::string ModMenuSpawner::spawn(const ModMenuSpawnEntry& entry, bool thenBecomeIt) { return pimpl->spawn(entry, thenBecomeIt); }
std::optional<ModMenuSpawner::Result> ModMenuSpawner::update() { return pimpl->update(); }
void ModMenuSpawner::onLevelChanged() { pimpl->onLevelChanged(); }
