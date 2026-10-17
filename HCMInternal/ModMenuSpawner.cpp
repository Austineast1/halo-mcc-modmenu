#include "pch.h"
#include "ModMenuSpawner.h"
#include "GameTickEventHook.h"
#include "TagTableRange.h"
#include "GetTagName.h"
#include "IMakeOrGetCheat.h"
#include "IGetMCCVersion.h"
#include "ModuleCache.h"
#include "MagicString.h"

namespace
{
	constexpr uint32_t kNone = 0xFFFFFFFF;
	constexpr uint32_t kCrashed = 0xFFFFFFFE;

	using PlacementNewFn = void(__fastcall*)(void* placement, uint32_t definition, uint32_t owner, void* damageOwner);
	using ObjectNewFn = uint32_t(__fastcall*)(void* placement);

	struct Layout
	{
		const wchar_t* module;
		uintptr_t placementNew, objectNew;
		std::vector<uint8_t> placementNewBytes, objectNewBytes;
		size_t placementSize;
		size_t placementPosition, placementForward;
	};

	std::optional<Layout> layoutFor(GameState game, std::string_view mccVersion)
	{
		if (game.operator GameState::Value() == GameState::Value::HaloReach && mccVersion == "1.3528.0.0")
		{
			return Layout{ L"haloreach.dll", 0x49CC24, 0x46D6E0,
				{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20,
				  0x41, 0x8B, 0xE8, 0x8B, 0xDA, 0x33, 0xD2, 0x41, 0xB8, 0xA8, 0x01, 0x00, 0x00, 0x49, 0x8B, 0xF1, 0x48, 0x8B, 0xF9, 0xE8 },
				{ 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00,
				  0x8B, 0x41, 0x18, 0x49, 0x83, 0xCC, 0xFF, 0x45, 0x33, 0xC0, 0xC1, 0xE8, 0x04, 0x44, 0x88, 0x44, 0x24, 0x50, 0x4C, 0x8B },
				0x1A8, 0x1C, 0x28 };
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
		return titleCase(last);
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
	int64_t mCatalogueRetryAtMs = 0;

	struct Request { ModMenuSpawnEntry entry; bool becomeIt; float position[3]; float forward[3]; };
	std::mutex mMutex;
	std::optional<Request> mPending;
	std::optional<Result> mResult;
	std::atomic_bool mFinished = false;
	int64_t mPendingSinceMs = 0;
	std::unique_ptr<ScopedCallback<eventpp::CallbackList<void(uint32_t)>>> mTickCallback;

	static int64_t nowMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
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
		return kind && *kind == entry.kind && (uint32_t)tag.value().tagDatum == entry.tag;
	}

	// game thread
	void onTick(uint32_t)
	{
		if (mFinished) return;
		std::optional<Request> request;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			request = mPending;
		}
		if (!request) return;

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
		mPending.reset();
		mResult = result;
		mFinished = true;
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
			// skip first-person arms, HUD-only and test content
			std::string lowered = path;
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			if (lowered.contains("fp_") || lowered.contains("first_person") || lowered.contains("\\fp\\") || lowered.contains("test")) continue;

			ModMenuSpawnEntry entry{ (uint32_t)tag.tagDatum, *kind, prettyName(path), path };
			nameCounts[entry.name]++;
			mCatalogue.push_back(std::move(entry));
		}
		// two tags with the same short name: add the folder so they're told apart
		for (auto& e : mCatalogue)
		{
			if (nameCounts[e.name] < 2) continue;
			std::string_view p = e.path;
			auto last = p.find_last_of("\\/");
			if (last == std::string_view::npos) continue;
			auto prev = p.substr(0, last).find_last_of("\\/");
			auto folder = p.substr(prev == std::string_view::npos ? 0 : prev + 1, last - (prev == std::string_view::npos ? 0 : prev + 1));
			if (titleCase(std::string(folder)) != e.name) e.name += " (" + titleCase(std::string(folder)) + ")";
		}
		std::sort(mCatalogue.begin(), mCatalogue.end(), [](const auto& a, const auto& b)
			{ return a.kind != b.kind ? a.kind < b.kind : a.name < b.name; });
		PLOG_INFO << "spawner: catalogue has " << mCatalogue.size() << " entries";
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
		if (!mCatalogueBuilt && nowMs() >= mCatalogueRetryAtMs)
		{
			try { buildCatalogue(); }
			catch (HCMRuntimeException ex) { PLOG_ERROR << "spawner: " << ex.what(); mCatalogue.clear(); }
			mCatalogueBuilt = !mCatalogue.empty();
			if (!mCatalogueBuilt) mCatalogueRetryAtMs = nowMs() + 3000; // level still loading, try again shortly
		}
		return mCatalogue;
	}

	std::string spawn(const ModMenuSpawnEntry& entry, bool becomeIt)
	{
		if (mPending) return "Still spawning the last one...";
		auto pose = mPose ? mPose() : std::nullopt;
		if (!pose) return "Load into a mission first.";

		float distance = spawnDistance(entry.kind), height = spawnHeight(entry.kind);
		Request request{ entry, becomeIt, {}, {} };
		request.forward[0] = std::cos(pose->yaw);
		request.forward[1] = std::sin(pose->yaw);
		request.forward[2] = 0.f;
		request.position[0] = pose->position.x + request.forward[0] * distance;
		request.position[1] = pose->position.y + request.forward[1] * distance;
		request.position[2] = pose->position.z + height;
		if (entry.kind == SpawnKind::Character)
		{
			// face the player so you can see who you spawned
			request.forward[0] = -request.forward[0];
			request.forward[1] = -request.forward[1];
		}

		{
			std::lock_guard<std::mutex> lock(mMutex);
			mPending = request;
			mResult.reset();
			mFinished = false;
			mPendingSinceMs = nowMs();
		}
		try
		{
			lockOrThrow(tickWeak, tick);
			mTickCallback = tick->getGameTickEvent()->subscribe([this](uint32_t t) { onTick(t); });
		}
		catch (HCMRuntimeException ex)
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mPending.reset();
			return std::format("Couldn't spawn: {}", ex.what());
		}
		return std::format("Spawning {}...", entry.name);
	}

	std::optional<Result> update()
	{
		if (!mFinished)
		{
			if (mPending && nowMs() - mPendingSinceMs > 15000)
			{
				std::lock_guard<std::mutex> lock(mMutex);
				mPending.reset();
				mTickCallback.reset();
				return Result{ "Nothing spawned because the game was paused. Try again in game.", kNone, false };
			}
			return std::nullopt;
		}
		mTickCallback.reset();
		mFinished = false;
		std::lock_guard<std::mutex> lock(mMutex);
		auto result = mResult;
		mResult.reset();
		return result;
	}

	// any thread
	void onLevelChanged()
	{
		mCatalogueStale = true;
		std::lock_guard<std::mutex> lock(mMutex);
		mPending.reset();
	}
};

ModMenuSpawner::ModMenuSpawner(GameState game, IDIContainer& dicon, std::function<std::optional<ModMenuPose>()> poseProvider)
	: pimpl(std::make_unique<Impl>(game, dicon, std::move(poseProvider))) {}
ModMenuSpawner::~ModMenuSpawner() = default;
const std::vector<ModMenuSpawnEntry>& ModMenuSpawner::catalogue() { return pimpl->catalogue(); }
std::string ModMenuSpawner::spawn(const ModMenuSpawnEntry& entry, bool thenBecomeIt) { return pimpl->spawn(entry, thenBecomeIt); }
std::optional<ModMenuSpawner::Result> ModMenuSpawner::update() { return pimpl->update(); }
void ModMenuSpawner::onLevelChanged() { pimpl->onLevelChanged(); }
