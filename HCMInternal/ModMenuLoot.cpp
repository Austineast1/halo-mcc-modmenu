#include "pch.h"
#include "ModMenuLoot.h"
#include "TagTableRange.h"
#include "TagBlockReader.h"
#include "GetTagName.h"
#include "IMakeOrGetCheat.h"
#include "IGetMCCVersion.h"
#include "ModuleCache.h"
#include "MagicString.h"
#include <atomic>
#include <chrono>
#include <map>
#include <random>
#include <set>

namespace
{
	constexpr uint32_t kNone = 0xFFFFFFFF;

	// weapon tag layout, Halo: Reach MCC 1.3528 (see ModMenuLoot.h)
	constexpr size_t kMagazines = 0x3F0, kMagazineSize = 0x60;
	constexpr size_t kMagTotalInitial = 0x6, kMagTotalMax = 0x8, kMagLoadedMax = 0xA;
	constexpr size_t kBarrels = 0x408, kBarrelSize = 0x184;
	constexpr size_t kRoundsPerSecond = 0x4, kFireRecovery = 0x28, kProjectilesPerShot = 0x6A;
	constexpr size_t kMinimumError = 0x70, kErrorAngle = 0x74, kProjectileRef = 0x104, kProjectileDatum = 0x110;

	// "is this tag loaded in the current zone set" (same check the spawner uses): never fire a projectile that isn't
	constexpr uintptr_t kTagAvailable = 0x8F620;
	const uint8_t kTagAvailableBytes[] = { 0x83, 0xF9, 0xFF, 0x74, 0x22, 0x48, 0x8B, 0x05, 0x24, 0x3F, 0xB0, 0x00 };
	using TagAvailableFn = bool(__fastcall*)(uint32_t tag);

	bool callTagAvailable(TagAvailableFn fn, uint32_t tag)
	{
		__try { return fn(tag); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	template <typename T> bool readSafe(uintptr_t address, T& out)
	{
		if (address < 0x10000 || IsBadReadPtr((void*)address, sizeof(T))) return false;
		memcpy(&out, (void*)address, sizeof(T));
		return true;
	}

	// tag data can sit in pages the game mapped read-only; open them up just long enough to write
	bool writeBytes(uintptr_t address, const void* data, size_t size)
	{
		if (address < 0x10000) return false;
		DWORD oldProtect = 0;
		bool reprotect = false;
		if (IsBadWritePtr((void*)address, size))
		{
			if (!VirtualProtect((void*)address, size, PAGE_READWRITE, &oldProtect)) return false;
			reprotect = true;
		}
		memcpy((void*)address, data, size);
		if (reprotect) VirtualProtect((void*)address, size, oldProtect, &oldProtect);
		return true;
	}

	std::string lowerCopy(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return s;
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

	std::string lastSegment(const std::string& path)
	{
		auto slash = path.find_last_of("\\/");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	// projectile path -> element name shown on the card
	std::string elementOf(const std::string& projectilePath)
	{
		auto p = lowerCopy(projectilePath);
		if (p.contains("needle")) return "Crystal";
		if (p.contains("fuel_rod") || p.contains("fuelrod")) return "Radiant";
		if (p.contains("concussion")) return "Shock";
		if (p.contains("rocket") || p.contains("grenade") || p.contains("launcher") || p.contains("explo") || p.contains("frag") || p.contains("mortar"))
			return "Explosive";
		if (p.contains("plasma") || p.contains("covenant") || p.contains("spike") || p.contains("focus") || p.contains("energy"))
			return "Plasma";
		return "Kinetic";
	}

	const char* kTitles[] = {
		"Hellfire", "Widowmaker", "Starfall", "Grunt Birthday Party", "Last Rites", "Thunderhead", "Halo Breaker",
		"Lightbringer", "Sunburst", "Banshee Scream", "Long Night", "Lone Wolf", "Hammer of Reach", "Glass Cannon",
		"Pillar of Autumn", "Ascendant Justice", "Covenant Killer", "Noble Sacrifice", "Sword of Sanghelios", "Zero Hour",
		"Winter's Edge", "The Great Journey", "Spirit of Fire", "Oni's Secret",
	};

	const char* kRarityNames[] = { "Common", "Uncommon", "Rare", "Epic", "Legendary" };
}

class ModMenuLoot::Impl
{
private:
	GameState mGame;
	std::weak_ptr<TagTableRange> tagTableWeak;
	std::weak_ptr<GetTagName> tagNameWeak;
	std::weak_ptr<TagBlockReader> tagBlockWeak;

	struct Patch { uintptr_t address; uint8_t size; uint64_t original; uint64_t value; };
	std::vector<Patch> mPatches;		// what we wrote, so restore() can undo exactly that
	std::vector<LootWeapon> mWeapons;
	uint32_t mSeed = 0;
	bool mAuto = false;
	std::atomic_bool mLevelChanged = false;
	int64_t mAutoRollAtMs = 0;

	struct Barrel
	{
		uintptr_t address;
		uint32_t projectile;
		float rps[2];
		float recovery;
		int16_t pps;
		float minError, error[2];
	};
	struct Gun
	{
		uint32_t tag;
		std::string path;
		std::string name;
		uintptr_t magazine = 0;	// first magazine element, 0 if none (batteries, swords)
		int16_t loaded = 0, total = 0, initial = 0;
		std::vector<Barrel> barrels;
	};

	static int64_t nowMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	std::optional<TagAvailableFn> tagAvailableCheck()
	{
		auto module = ModuleCache::getModuleHandle(L"haloreach.dll");
		if (!module) return std::nullopt;
		auto address = (uintptr_t)module.value() + kTagAvailable;
		if (IsBadReadPtr((void*)address, sizeof(kTagAvailableBytes)) || memcmp((void*)address, kTagAvailableBytes, sizeof(kTagAvailableBytes)) != 0)
			return std::nullopt;
		return (TagAvailableFn)address;
	}

	std::optional<ParsedTagBlock> block(TagBlockReader& reader, uintptr_t address)
	{
		auto parsed = reader.read(address);
		if (!parsed) return std::nullopt;
		return parsed.value();
	}

	// every handheld gun with at least one barrel that fires a projectile
	std::vector<Gun> findGuns(std::map<uint32_t, std::string>& projectilePaths)
	{
		std::vector<Gun> guns;
		auto tagTable = tagTableWeak.lock();
		auto tagName = tagNameWeak.lock();
		auto tagBlocks = tagBlockWeak.lock();
		if (!tagTable || !tagName || !tagBlocks) return guns;
		auto tags = tagTable->getTagTableRange();
		if (!tags) { PLOG_ERROR << "loot: couldn't read the tag table: " << tags.error().what(); return guns; }

		const int32_t weap = MagicString(std::string("weap")).getNum();
		const int32_t proj = MagicString(std::string("proj")).getNum();
		auto available = tagAvailableCheck();

		auto nameOf = [&](uint32_t datum) -> std::string
			{
				try
				{
					const char* name = tagName->getTagName(Datum((uint16_t)(datum & 0xFFFF), (uint16_t)(datum >> 16)));
					return name ? std::string(name) : std::string();
				}
				catch (HCMRuntimeException) { return std::string(); }
			};

		for (auto& tag : tags.value())
		{
			if (tag.tagType.getNum() != weap) continue;
			uint32_t datum = (uint32_t)tag.tagDatum;
			Gun gun{ datum };
			gun.path = nameOf(datum);
			auto lowered = lowerCopy(gun.path);
			if (gun.path.empty() || lowered.contains("unarmed") || lowered.contains("first_person") || lowered.contains("\\fp\\") || lowered.contains("test")) continue;
			// guns bolted to vehicles (Warthog chaingun, Banshee cannons) stay as they are; detachable turrets are fair game
			if (lowered.starts_with("objects\\vehicles\\") && !lastSegment(lowered).contains("turret")) continue;
			gun.name = titleCase(lastSegment(gun.path));

			uintptr_t base = tag.tagAddress;
			if (auto mags = block(*tagBlocks, base + kMagazines); mags && mags->elementCount > 0)
			{
				gun.magazine = mags->firstElement;
				readSafe(gun.magazine + kMagLoadedMax, gun.loaded);
				readSafe(gun.magazine + kMagTotalMax, gun.total);
				readSafe(gun.magazine + kMagTotalInitial, gun.initial);
			}
			auto barrels = block(*tagBlocks, base + kBarrels);
			if (!barrels) continue;
			for (size_t i = 0; i < barrels->elementCount && i < 4; i++)
			{
				Barrel b{ barrels->firstElement + i * kBarrelSize };
				int32_t group = 0;
				if (!readSafe(b.address + kProjectileRef, group) || group != proj) continue;
				if (!readSafe(b.address + kProjectileDatum, b.projectile) || b.projectile == kNone) continue;
				if (!readSafe(b.address + kRoundsPerSecond, b.rps) || !readSafe(b.address + kFireRecovery, b.recovery)) continue;
				if (!readSafe(b.address + kProjectilesPerShot, b.pps) || !readSafe(b.address + kMinimumError, b.minError) || !readSafe(b.address + kErrorAngle, b.error)) continue;
				if (!std::isfinite(b.rps[0]) || !std::isfinite(b.rps[1]) || b.pps < 1 || b.pps > 64) continue;
				gun.barrels.push_back(b);
				if (!projectilePaths.contains(b.projectile))
				{
					if (available && !callTagAvailable(*available, b.projectile)) continue;
					projectilePaths[b.projectile] = nameOf(b.projectile);
				}
			}
			if (!gun.barrels.empty()) guns.push_back(std::move(gun));
		}
		return guns;
	}

	template <typename T> void patch(uintptr_t address, T value)
	{
		T original{};
		if (!readSafe(address, original)) return;
		if (memcmp(&original, &value, sizeof(T)) == 0) return;
		Patch p{ address, (uint8_t)sizeof(T), 0, 0 };
		memcpy(&p.original, &original, sizeof(T));
		memcpy(&p.value, &value, sizeof(T));
		if (writeBytes(address, &value, sizeof(T))) mPatches.push_back(p);
	}

	// put back everything we changed, but only where our value is still there (a reloaded level has fresh tags)
	int undoPatches()
	{
		int restored = 0;
		for (auto it = mPatches.rbegin(); it != mPatches.rend(); ++it)
		{
			uint64_t current = 0;
			if (IsBadReadPtr((void*)it->address, it->size)) continue;
			memcpy(&current, (void*)it->address, it->size);
			if (memcmp(&current, &it->value, it->size) != 0) continue;
			if (writeBytes(it->address, &it->original, it->size)) restored++;
		}
		mPatches.clear();
		return restored;
	}

	struct Roll
	{
		float rate = 1.f, mag = 1.f, accuracy = 1.f;	// accuracy < 1 = tighter
		int ppsMul = 1;
		std::string element;
		std::vector<std::string> perks;
		std::string adjective;
	};

public:
	Impl(GameState game, IDIContainer& dicon) : mGame(game)
	{
		auto version = dicon.Resolve<IGetMCCVersion>().lock();
		if (game.operator GameState::Value() != GameState::Value::HaloReach || !version || version->getMCCVersionAsString() != "1.3528.0.0")
			throw HCMInitException("Loot only knows Halo: Reach's weapon layout (MCC 1.3528) so far");
		tagTableWeak = resolveDependentCheat(TagTableRange);
		tagNameWeak = resolveDependentCheat(GetTagName);
		tagBlockWeak = resolveDependentCheat(TagBlockReader);
	}

	const std::vector<LootWeapon>& weapons() const { return mWeapons; }
	bool active() const { return !mWeapons.empty(); }
	uint32_t seed() const { return mSeed; }
	void setAutoReroll(bool on) { mAuto = on; }
	bool autoReroll() const { return mAuto; }

	std::string roll(std::optional<uint32_t> seed)
	{
		// start from the level's own values so re-rolls don't stack
		undoPatches();
		mWeapons.clear();

		std::map<uint32_t, std::string> projectilePaths;
		std::vector<Gun> guns;
		try { guns = findGuns(projectilePaths); }
		catch (HCMRuntimeException ex) { return std::format("Couldn't read the weapons: {}", ex.what()); }
		if (guns.empty()) return "No guns found yet. Load into a mission first.";

		// elements: every projectile some gun here already fires, grouped by what it looks like
		std::map<std::string, std::vector<uint32_t>> elements;
		for (auto& [datum, path] : projectilePaths) elements[elementOf(path)].push_back(datum);

		mSeed = seed.value_or((uint32_t)std::random_device{}());
		std::mt19937 rng(mSeed);
		auto uniform = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
		auto chance = [&](float p) { return uniform(0.f, 1.f) < p; };
		auto pick = [&](size_t n) { return std::uniform_int_distribution<size_t>(0, n - 1)(rng); };

		struct Maker { const char* name; const char* favouredElement; int favouredPerk; };
		static const Maker makers[] = {
			{ "Misriah Armory", "Kinetic", 1 },		// big magazines, steady
			{ "Iruiru Armory", nullptr, 0 },		// fire rate
			{ "Sangheili Forge", "Plasma", 4 },		// plasma, overcharged
			{ "Kig-Yar Salvage", nullptr, 3 },		// scatter, anything goes
			{ "Forerunner Relic", "Radiant", 5 },	// legendary only
		};
		static const float rarityWeights[] = { 40.f, 30.f, 17.f, 9.f, 4.f };

		int counts[(int)LootRarity::Count] = {};
		for (auto& gun : guns)
		{
			int r = (int)std::discrete_distribution<int>(std::begin(rarityWeights), std::end(rarityWeights))(rng);
			auto rarity = (LootRarity)r;
			counts[r]++;
			const Maker& maker = rarity == LootRarity::Legendary && chance(0.4f) ? makers[4] : makers[pick(4)];

			static const float rateLo[] = { 1.0f, 1.05f, 1.1f, 1.2f, 1.3f }, rateHi[] = { 1.1f, 1.25f, 1.35f, 1.5f, 1.7f };
			static const float magLo[] = { 1.0f, 1.2f, 1.4f, 1.7f, 2.0f }, magHi[] = { 1.25f, 1.6f, 2.0f, 2.6f, 3.5f };
			static const float elementChance[] = { 0.f, 0.2f, 0.45f, 0.7f, 1.f };
			static const int perkCount[] = { 0, 1, 1, 2, 2 };

			Roll roll;
			roll.rate = uniform(rateLo[r], rateHi[r]);
			roll.mag = uniform(magLo[r], magHi[r]);

			// perks: 0 rapid, 1 deep mag, 2 twin, 3 scatter, 4 overcharged, 5 deadeye
			std::set<int> perks;
			int wanted = perkCount[r] + (r == 2 && chance(0.5f) ? 1 : 0);
			if (wanted > 0 && chance(0.6f)) perks.insert(maker.favouredPerk);
			while ((int)perks.size() < wanted) perks.insert((int)pick(6));
			if (rarity != LootRarity::Legendary) perks.erase(4); // overcharged is a legendary thing
			for (int perk : perks)
			{
				switch (perk)
				{
				case 0: { float x = uniform(1.35f, 1.8f); roll.rate *= x; roll.perks.push_back(std::format("Rapid: fires {:.0f}% faster", (x - 1.f) * 100.f)); roll.adjective = "Rapid"; break; }
				case 1: { float x = uniform(1.8f, 3.f); roll.mag *= x; roll.perks.push_back(std::format("Deep Mag: magazine x{:.1f}", x)); roll.adjective = "Bottomless"; break; }
				case 2: roll.ppsMul *= 2; roll.accuracy *= 1.4f; roll.perks.push_back("Twin: 2 rounds per shot"); roll.adjective = "Twin"; break;
				case 3: roll.ppsMul *= 5; roll.accuracy *= 2.6f; roll.perks.push_back("Scatter: shotgun spread, 5 rounds per shot"); roll.adjective = "Scattered"; break;
				case 4: roll.ppsMul *= 3; roll.rate *= 1.25f; roll.perks.push_back("Overcharged: 3 rounds per shot, 25% faster"); roll.adjective = "Overcharged"; break;
				case 5: roll.accuracy *= 0.15f; roll.perks.push_back("Deadeye: laser accurate"); roll.adjective = "Deadeye"; break;
				}
			}

			// element: swap in a projectile from another gun on this level
			if (chance(elementChance[r]) && !elements.empty())
			{
				std::string want = maker.favouredElement && chance(0.6f) && elements.contains(maker.favouredElement) ? maker.favouredElement : "";
				if (want.empty())
				{
					auto it = elements.begin();
					std::advance(it, pick(elements.size()));
					want = it->first;
				}
				roll.element = want;
			}

			// write it
			LootWeapon loot;
			loot.tag = gun.tag;
			loot.baseName = gun.name;
			loot.path = gun.path;
			loot.rarity = rarity;
			loot.maker = maker.name;
			if (rarity == LootRarity::Legendary || (rarity == LootRarity::Epic && chance(0.35f))) loot.title = kTitles[pick(std::size(kTitles))];

			int oldPps = gun.barrels.front().pps, newPps = oldPps;
			float oldRps = gun.barrels.front().rps[1];
			bool swapped = false;
			std::optional<uint32_t> elementProjectile;
			if (!roll.element.empty())
			{
				auto& pool = elements[roll.element];
				elementProjectile = pool[pick(pool.size())];
			}
			for (auto& b : gun.barrels)
			{
				float rate = std::min(roll.rate, 3.5f);
				float rps[2] = { b.rps[0] * rate, b.rps[1] * rate };
				for (float& v : rps) v = std::min(v, std::max(50.f, std::max(b.rps[0], b.rps[1])));
				int pps = std::clamp(b.pps * roll.ppsMul, 1, 12);
				// keep the game sane: no more than ~120 projectiles a second out of one barrel
				while (pps > 1 && rps[1] * pps > 120.f) pps--;
				float accuracy = roll.accuracy;
				float minError = b.minError * accuracy, error0 = b.error[0] * accuracy, error1 = b.error[1] * accuracy;
				if (pps > b.pps)
				{
					// several rounds per shot need some spread or they all land in one hole
					float floor = 0.012f * pps;
					minError = std::max(minError, floor * 0.5f);
					error1 = std::max(error1, floor);
					error0 = std::max(error0, floor * 0.6f);
				}
				patch<float>(b.address + kRoundsPerSecond, rps[0]);
				patch<float>(b.address + kRoundsPerSecond + 4, rps[1]);
				if (b.recovery > 0.f) patch<float>(b.address + kFireRecovery, b.recovery / rate);
				patch<int16_t>(b.address + kProjectilesPerShot, (int16_t)pps);
				patch<float>(b.address + kMinimumError, minError);
				patch<float>(b.address + kErrorAngle, error0);
				patch<float>(b.address + kErrorAngle + 4, error1);
				if (elementProjectile && *elementProjectile != b.projectile)
				{
					patch<uint32_t>(b.address + kProjectileDatum, *elementProjectile);
					swapped = true;
				}
				if (&b == &gun.barrels.front()) newPps = pps;
			}
			int newLoaded = gun.loaded;
			if (gun.magazine && gun.loaded > 0)
			{
				float mag = std::min(roll.mag, 12.f);
				newLoaded = std::clamp((int)std::lround(gun.loaded * mag), (int)gun.loaded, 999);
				int newTotal = std::clamp((int)std::lround(gun.total * mag), (int)gun.total, 9999);
				int newInitial = std::clamp((int)std::lround(gun.initial * mag), (int)gun.initial, newTotal);
				patch<int16_t>(gun.magazine + kMagLoadedMax, (int16_t)newLoaded);
				patch<int16_t>(gun.magazine + kMagTotalMax, (int16_t)newTotal);
				patch<int16_t>(gun.magazine + kMagTotalInitial, (int16_t)newInitial);
			}

			loot.element = swapped ? roll.element : "";
			loot.stats.push_back({ "Fire rate", std::format("x{:.2f}", std::min(roll.rate, 3.5f)), roll.rate >= 1.f });
			if (gun.magazine && gun.loaded > 0) loot.stats.push_back({ "Magazine", std::format("{} > {}", gun.loaded, newLoaded), newLoaded >= gun.loaded });
			if (newPps != oldPps) loot.stats.push_back({ "Rounds per shot", std::format("{} > {}", oldPps, newPps), true });
			if (roll.accuracy < 0.95f) loot.stats.push_back({ "Accuracy", std::format("+{:.0f}%", (1.f - roll.accuracy) * 100.f), true });
			else if (roll.accuracy > 1.05f) loot.stats.push_back({ "Spread", std::format("x{:.1f}", roll.accuracy), false });
			if (swapped) loot.perks.push_back(std::format("{} rounds: fires {}", roll.element, titleCase(lastSegment(projectilePaths[*elementProjectile]))));
			for (auto& p : roll.perks) loot.perks.push_back(p);
			(void)oldRps;
			if (loot.title.empty() && !roll.adjective.empty()) loot.title = roll.adjective;
			mWeapons.push_back(std::move(loot));
		}

		// best first, like a loot screen
		std::stable_sort(mWeapons.begin(), mWeapons.end(), [](const LootWeapon& a, const LootWeapon& b) { return (int)a.rarity > (int)b.rarity; });
		PLOG_INFO << "loot: rolled " << mWeapons.size() << " guns with seed " << mSeed << ", " << mPatches.size() << " values written";

		std::string top;
		for (auto& w : mWeapons)
			if (w.rarity == LootRarity::Legendary) { top = std::format(" Legendary: {} {}.", w.title, w.baseName); break; }
		return std::format("Rolled {} guns: {} legendary, {} epic, {} rare.{}", mWeapons.size(),
			counts[(int)LootRarity::Legendary], counts[(int)LootRarity::Epic], counts[(int)LootRarity::Rare], top);
	}

	std::string restore()
	{
		int restored = undoPatches();
		bool had = !mWeapons.empty();
		mWeapons.clear();
		if (!had) return "Every gun is already standard issue.";
		return std::format("Every gun is back to standard issue ({} values restored).", restored);
	}

	std::optional<std::string> update(bool gamePlaying)
	{
		if (mLevelChanged.exchange(false))
		{
			// the level's tags were reloaded from disk: our values are gone, so forget them without writing anything
			mPatches.clear();
			mWeapons.clear();
			mAutoRollAtMs = mAuto ? nowMs() + 4000 : 0;
		}
		if (mAutoRollAtMs != 0 && gamePlaying && nowMs() >= mAutoRollAtMs)
		{
			mAutoRollAtMs = 0;
			auto message = roll(std::nullopt);
			if (mWeapons.empty()) { mAutoRollAtMs = nowMs() + 3000; return std::nullopt; } // still loading
			return "New loot for this mission. " + message;
		}
		return std::nullopt;
	}

	void onLevelChanged() { mLevelChanged = true; }

	std::vector<std::string> describeGuns()
	{
		std::vector<std::string> out;
		std::map<uint32_t, std::string> projectiles;
		for (auto& gun : findGuns(projectiles))
		{
			std::string line = std::format("{:08X} {} mag {}/{}", gun.tag, gun.path, gun.loaded, gun.total);
			for (auto& b : gun.barrels)
				line += std::format(" | rps {:.1f}-{:.1f} pps {} err {:.3f}/{:.3f}-{:.3f} proj {:08X}", b.rps[0], b.rps[1], b.pps, b.minError, b.error[0], b.error[1], b.projectile);
			out.push_back(line);
		}
		for (auto& [datum, path] : projectiles) out.push_back(std::format("projectile {:08X} {} = {}", datum, elementOf(path), path));
		return out;
	}
};

ModMenuLoot::ModMenuLoot(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
ModMenuLoot::~ModMenuLoot() = default;
std::string ModMenuLoot::roll(std::optional<uint32_t> seed) { return pimpl->roll(seed); }
std::string ModMenuLoot::restore() { return pimpl->restore(); }
const std::vector<LootWeapon>& ModMenuLoot::weapons() const { return pimpl->weapons(); }
bool ModMenuLoot::active() const { return pimpl->active(); }
uint32_t ModMenuLoot::seed() const { return pimpl->seed(); }
void ModMenuLoot::setAutoReroll(bool on) { pimpl->setAutoReroll(on); }
bool ModMenuLoot::autoReroll() const { return pimpl->autoReroll(); }
std::optional<std::string> ModMenuLoot::update(bool gamePlaying) { return pimpl->update(gamePlaying); }
void ModMenuLoot::onLevelChanged() { pimpl->onLevelChanged(); }
std::vector<std::string> ModMenuLoot::describeGuns() { return pimpl->describeGuns(); }
