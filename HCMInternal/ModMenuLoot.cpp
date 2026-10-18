#include "pch.h"
#include "ModMenuLoot.h"
#include "TagTableRange.h"
#include "TagBlockReader.h"
#include "GetTagName.h"
#include "IMakeOrGetCheat.h"
#include "IGetMCCVersion.h"
#include "ModuleCache.h"
#include "MagicString.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"
#include "DirPathContainer.h"
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>
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

	// projectile -> damage effect (ReachMCC proj plugin): tag references are {group, ?, ?, datum}. Bullets hurt through
	// "Impact Damage", grenades and bolts through "Detonation Damage". Damage effect (jpt!): lower bound +0x20, upper
	// bound range +0x24/+0x28. Every value is sanity-checked before it's scaled, and skipped if it looks wrong.
	constexpr size_t kProjImpactDamage = 0x284, kProjDetonationDamage = 0x1D8, kTagRefDatum = 0xC;

	// more barrel fields: ammo used per shot (0 = never runs dry), distribution function (1 = horizontal fan) and fan angle
	constexpr size_t kRoundsUsedPerShot = 0x3A, kDistributionFunction = 0x68, kDistributionAngle = 0x6C;
	// weapon autoaim (helps seeking rounds find a target): angle (radians) +0x2D4, range +0x2D8
	constexpr size_t kAutoaimAngle = 0x2D4, kAutoaimRange = 0x2D8;
	// projectile flight (ReachMCC proj plugin): collision radius +0x170, air gravity scale +0x2B8, initial/final velocity
	// +0x2D0/+0x2D4, guided angular velocity lower/upper/at rest +0x2F0/+0x2F4/+0x2F8 (radians per second).
	// Material responses: new block +0x320 (0x34 each: flags +0x6, chance +0x8, angle range +0xC, response +0x1C,
	// frictions +0x2C/+0x30) and old block +0x314 (0x3C each: flags +0xE, chance +0x10, angle range +0x14, potential
	// response +0xC, frictions +0x34/+0x38). Response 4 = bounce; flag bit 1 = never against units.
	constexpr size_t kProjCollisionRadius = 0x170, kProjGravity = 0x2B8, kProjVelocity = 0x2D0, kProjGuided = 0x2F0;
	constexpr size_t kProjResponses = 0x320, kProjResponseSize = 0x34, kProjOldResponses = 0x314, kProjOldResponseSize = 0x3C;
	constexpr size_t kDamageLower = 0x20, kDamageUpper = 0x24;

	// which gun the player is holding (Reach unit layout, found live Oct 2026 by switching weapons and diffing the unit):
	// current weapon slot (byte) at unit +0x34A (+0x34E follows it; +0x348 does not change), the four carried weapon
	// datums at +0x350. A weapon object's parent (+0x14) is the unit carrying it.
	constexpr size_t kUnitWeaponSlot = 0x34A, kUnitWeapons = 0x350, kObjectParent = 0x14;
	constexpr uint8_t kWeaponObjectType = 2;

	// who killed what (haloreach.dll 1.3528, static analysis of object_set_damage_owner / the body-depleted handler):
	// every hit copies the 12-byte damage owner {player, unit, team} to object +0xF0 and the damage effect's datum to
	// +0x154; object +0x138 bit 2 is set when the object dies. Bipeds keep all three until their body is cleaned up.
	constexpr size_t kObjectDamageOwner = 0xF0, kObjectLastDamageEffect = 0x154, kObjectDamageFlags = 0x138;
	constexpr uint32_t kObjectDeadFlag = 0x4;
	constexpr uint8_t kBipedObjectType = 0;
	constexpr size_t kObjectSlots = 2048;
	constexpr size_t kPlayerUnitField = 0x28;	// player record +0x28 = unit datum; the record starts with its salt

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
		// acronyms
		for (const char* word : { "Dmr", "Smg", "Odst", "Unsc" })
		{
			std::string w = word, up = w;
			std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return (char)std::toupper(c); });
			for (size_t pos = s.find(w); pos != std::string::npos; pos = s.find(w, pos + w.size()))
			{
				bool startOk = pos == 0 || s[pos - 1] == ' ';
				bool endOk = pos + w.size() == s.size() || s[pos + w.size()] == ' ';
				if (startOk && endOk) s.replace(pos, w.size(), up);
			}
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

	// ---- weapon mastery
	std::string mMasteryPath;					// ModMenuMastery.txt, next to the menu's other files
	std::map<std::string, int> mKills;			// lower-case gun tag path -> kills
	std::vector<LootWeapon> mStandard;			// cards for guns that only have mastery (loot is off)
	uint32_t mCardVersion = 1;
	uint32_t mLevelUpTag = kNone, mLevelUpVersion = 0;
	int64_t mMasteryApplyAtMs = 0;
	int64_t mNextKillScanMs = 0;
	std::unordered_map<uint32_t, bool> mDead;	// biped datum -> was it dead last time we looked
	bool mScanPrimed = false;
	std::vector<uint8_t> mHeaderCopy;
	std::map<uint32_t, std::set<uint32_t>> mGunDamageCache;	// gun tag -> damage effects its rounds deal right now
	ModMenuLoot::MasteryKill mLastKill;
	bool mHaveKill = false;

	// object lookups for heldWeapon() (optional: the card just doesn't show without them)
	std::shared_ptr<MultilevelPointer> playerUnitPointer, objectMetaDataTable;
	int64_t headerStride = 0x18, headerType = 0x4, headerPointer = 0x10;

	uintptr_t objectAddress(uint32_t datum, std::optional<uint8_t> type = std::nullopt)
	{
		if (datum == kNone || !objectMetaDataTable) return 0;
		uintptr_t meta = 0;
		if (!objectMetaDataTable->resolve(&meta)) return 0;
		uint16_t index = (uint16_t)(datum & 0xFFFF), salt = (uint16_t)(datum >> 16);
		if (index >= 2048) return 0;
		uintptr_t header = meta + headerStride * index;
		uint16_t headerSalt = 0;
		uint8_t headerKind = 0xFF;
		uintptr_t address = 0;
		if (!readSafe(header, headerSalt) || headerSalt != salt) return 0;
		if (type && (!readSafe(header + headerType, headerKind) || headerKind != *type)) return 0;
		if (!readSafe(header + headerPointer, address)) return 0;
		return IsBadReadPtr((void*)address, 0x400) ? 0 : address;
	}

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
		uintptr_t base = 0;		// the weapon tag itself
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
			gun.base = base;
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
		mGunDamageCache.clear();
		return restored;
	}

	// the damage effect a projectile deals (impact first, else detonation), with its bounds looking like real damage
	struct DamageEffect { uint32_t datum; uintptr_t address; };
	std::optional<DamageEffect> damageEffectOf(uint32_t projectile)
	{
		auto tagTable = tagTableWeak.lock();
		if (!tagTable || projectile == kNone) return std::nullopt;
		static const int32_t projGroup = MagicString(std::string("proj")).getNum();
		static const int32_t jptGroup = MagicString(std::string("jpt!")).getNum();
		auto projTag = tagTable->getTagByDatum(Datum((uint16_t)(projectile & 0xFFFF), (uint16_t)(projectile >> 16)));
		if (!projTag || projTag.value().tagType.getNum() != projGroup) return std::nullopt;
		for (size_t refOffset : { kProjImpactDamage, kProjDetonationDamage })
		{
			int32_t group = 0;
			uint32_t datum = kNone;
			uintptr_t ref = projTag.value().tagAddress + refOffset;
			if (!readSafe(ref, group) || group != jptGroup || !readSafe(ref + kTagRefDatum, datum) || datum == kNone) continue;
			auto jpt = tagTable->getTagByDatum(Datum((uint16_t)(datum & 0xFFFF), (uint16_t)(datum >> 16)));
			if (!jpt || jpt.value().tagType.getNum() != jptGroup) continue;
			float lower = 0.f, upper[2] = {};
			uintptr_t a = jpt.value().tagAddress;
			if (!readSafe(a + kDamageLower, lower) || !readSafe(a + kDamageUpper, upper)) continue;
			auto sane = [](float v) { return std::isfinite(v) && v >= 0.f && v <= 5000.f; };
			if (!sane(lower) || !sane(upper[0]) || !sane(upper[1]) || upper[1] <= 0.f || lower > upper[1] + 0.001f || upper[0] > upper[1] + 0.001f) continue;
			return DamageEffect{ datum, a };
		}
		return std::nullopt;
	}

	struct Roll
	{
		float damage = 1.f;
		float rate = 1.f, mag = 1.f, accuracy = 1.f;	// accuracy < 1 = tighter
		int ppsMul = 1;
		std::string element;
		std::vector<std::string> perks;
		std::string adjective;
		std::set<int> flight;		// projectile perks (7..13), applied to whatever this gun ends up firing
		bool endless = false;		// legendary special: never uses ammo
		bool fan = false;
	};

	// ---- projectile perks (shared by every gun that fires the same projectile, so the notes are kept per projectile)
	std::map<uint32_t, std::vector<std::string>> mProjectileNotes;
	std::set<std::pair<uint32_t, int>> mFlightApplied;	// (projectile, perk) already written this roll

	std::optional<uintptr_t> projectileTag(uint32_t datum)
	{
		auto tagTable = tagTableWeak.lock();
		if (!tagTable || datum == kNone) return std::nullopt;
		static const int32_t projGroup = MagicString(std::string("proj")).getNum();
		auto tag = tagTable->getTagByDatum(Datum((uint16_t)(datum & 0xFFFF), (uint16_t)(datum >> 16)));
		if (!tag || tag.value().tagType.getNum() != projGroup) return std::nullopt;
		return tag.value().tagAddress;
	}

	static bool sane(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }

	// rounds bounce off the world (still hit people): every impact/fizzle response becomes a bounce that ignores units
	bool makeRicochet(uintptr_t proj)
	{
		auto tagBlocks = tagBlockWeak.lock();
		if (!tagBlocks) return false;
		bool any = false;
		auto convert = [&](uintptr_t e, size_t flagsAt, size_t chanceAt, size_t angleAt, size_t responseAt, size_t frictionAt)
			{
				int16_t response = -1;
				uint16_t flags = 0;
				if (!readSafe(e + responseAt, response) || !readSafe(e + flagsAt, flags)) return;
				if (response != 0 && response != 1 && response != 6) return;	// impact, fizzle, ricochet-fizzle
				if (flags & 0x1) return;										// only-against-units responses stay
				float friction[2] = {};
				if (!readSafe(e + frictionAt, friction) || !sane(friction[0], 0.f, 1.f) || !sane(friction[1], 0.f, 1.f)) return;
				patch<int16_t>(e + responseAt, 4);
				patch<uint16_t>(e + flagsAt, (uint16_t)(flags | 0x2));
				patch<float>(e + chanceAt, 1.f);
				patch<float>(e + angleAt, 0.f);
				patch<float>(e + angleAt + 4, 1.5708f);
				patch<float>(e + frictionAt, std::min(friction[0], 0.1f));
				patch<float>(e + frictionAt + 4, std::min(friction[1], 0.25f));
				any = true;
			};
		if (auto blk = block(*tagBlocks, proj + kProjResponses); blk && blk->elementCount < 64)
			for (size_t i = 0; i < blk->elementCount; i++) convert(blk->firstElement + i * kProjResponseSize, 0x6, 0x8, 0xC, 0x1C, 0x2C);
		if (auto blk = block(*tagBlocks, proj + kProjOldResponses); blk && blk->elementCount < 64)
			for (size_t i = 0; i < blk->elementCount; i++) convert(blk->firstElement + i * kProjOldResponseSize, 0xE, 0x10, 0x14, 0xC, 0x34);
		return any;
	}

	// apply one projectile perk to a projectile tag; returns the line for the card, or empty if it couldn't be done
	std::string applyFlight(uint32_t projectile, int perk)
	{
		auto proj = projectileTag(projectile);
		if (!proj) return "";
		uintptr_t p = *proj;
		float gravity = 0.f, velocity[2] = {}, radius = 0.f, guided[3] = {};
		if (!readSafe(p + kProjGravity, gravity) || !readSafe(p + kProjVelocity, velocity) || !readSafe(p + kProjCollisionRadius, radius) || !readSafe(p + kProjGuided, guided))
			return "";
		if (!sane(gravity, 0.f, 50.f) || !sane(velocity[0], 0.f, 5000.f) || !sane(velocity[1], 0.f, 5000.f) || !sane(radius, 0.f, 10.f)
			|| !sane(guided[0], 0.f, 100.f) || !sane(guided[1], 0.f, 100.f) || !sane(guided[2], 0.f, 100.f))
			return "";
		switch (perk)
		{
		case 7: return makeRicochet(p) ? "Ricochet: rounds bounce off walls" : "";
		case 8:
			patch<float>(p + kProjGuided, std::max(guided[0], 2.5f));
			patch<float>(p + kProjGuided + 4, std::max(guided[1], 3.5f));
			patch<float>(p + kProjGuided + 8, std::max(guided[2], 2.0f));
			return "Seeker: rounds curve into whatever you aim near";
		case 9:
			if (velocity[0] <= 0.f) return "";
			patch<float>(p + kProjVelocity, std::min(velocity[0] * 3.f, 1500.f));
			patch<float>(p + kProjVelocity + 4, std::min(velocity[1] * 3.f, 1500.f));
			patch<float>(p + kProjGravity, 0.f);
			return "Railgun: hyper-velocity slugs, dead straight";
		case 10:
			if (velocity[0] <= 0.f) return "";
			patch<float>(p + kProjVelocity, std::max(velocity[0] * 0.3f, 3.f));
			patch<float>(p + kProjVelocity + 4, std::max(velocity[1] * 0.3f, 3.f));
			patch<float>(p + kProjGravity, 0.f);
			return "Drifter: slow rounds that float through the air";
		case 11:
			if (velocity[0] <= 0.f) return "";
			patch<float>(p + kProjGravity, std::min(std::max(gravity, 1.f) * 2.5f, 6.f));
			patch<float>(p + kProjVelocity, std::max(velocity[0] * 0.6f, 5.f));
			patch<float>(p + kProjVelocity + 4, std::max(velocity[1] * 0.6f, 5.f));
			return "Mortar: rounds arc down like grenades";
		case 12:
			patch<float>(p + kProjCollisionRadius, std::min(std::max(radius * 4.f, 0.06f), 0.6f));
			return "Big Bore: fat rounds that are hard to miss";
		}
		return "";
	}

	// ---- weapon mastery -------------------------------------------------------------------------------------------
	std::deque<std::string> mRecentDeaths;
	static uint32_t readTagOf(uintptr_t object) { uint32_t tag = kNone; readSafe(object, tag); return tag; }
	std::set<std::string> mNoMagazine;		// gun paths with no magazine (batteries, swords): no magazine bonus
	std::optional<std::string> mPendingMessage;
	int64_t mLevelUpAtMs = 0;

	static std::string masteryKey(const std::string& path) { return lowerCopy(path); }

	int killsFor(const std::string& path) const
	{
		auto it = mKills.find(masteryKey(path));
		return it == mKills.end() ? 0 : it->second;
	}

	static LootMastery masteryFrom(int kills, bool hasMagazine)
	{
		LootMastery m;
		m.kills = kills;
		m.level = masteryLevelFor(kills);
		m.levelKills = kMasteryKills[m.level];
		m.nextKills = m.level >= kMasteryMaxLevel ? 0 : kMasteryKills[m.level + 1];
		m.rank = kMasteryRanks[m.level];
		if (m.level > 0)
		{
			auto b = masteryBonus(m.level);
			m.bonuses.push_back(std::format("+{:.0f}% fire rate", (b.rate - 1.f) * 100.f));
			if (hasMagazine) m.bonuses.push_back(std::format("+{:.0f}% magazine", (b.mag - 1.f) * 100.f));
			m.bonuses.push_back(std::format("{:.0f}% tighter spread", (1.f - b.accuracy) * 100.f));
		}
		return m;
	}

	LootMastery masteryOf(const std::string& path) const
	{
		return masteryFrom(killsFor(path), !mNoMagazine.contains(masteryKey(path)));
	}

	void loadMastery()
	{
		mKills.clear();
		if (mMasteryPath.empty()) return;
		std::ifstream in(mMasteryPath);
		std::string line;
		while (std::getline(in, line))
		{
			if (line.starts_with("#")) continue;
			auto tab = line.find('\t');
			if (tab == std::string::npos) continue;
			std::string path = line.substr(tab + 1);
			while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) path.pop_back();
			int kills = 0;
			try { kills = std::stoi(line.substr(0, tab)); }
			catch (...) { continue; }
			if (kills > 0 && !path.empty()) mKills[masteryKey(path)] = std::min(kills, 1000000);
		}
		PLOG_INFO << "loot: weapon mastery loaded for " << mKills.size() << " guns";
	}

	void saveMastery()
	{
		if (mMasteryPath.empty()) return;
		std::string temp = mMasteryPath + ".tmp";
		{
			std::ofstream out(temp, std::ios::trunc);
			if (!out) return;
			out << "# Mod Menu weapon mastery: kills, a tab, then the gun. Delete this file to start over.\n";
			for (auto& [path, kills] : mKills)
				if (kills > 0) out << kills << '\t' << path << '\n';
		}
		std::error_code ec;
		std::filesystem::rename(temp, mMasteryPath, ec);
		if (ec)
		{
			std::filesystem::remove(mMasteryPath, ec);
			std::filesystem::rename(temp, mMasteryPath, ec);
		}
	}

	uint32_t playerUnit()
	{
		if (!playerUnitPointer) return kNone;
		uintptr_t unitField = 0;
		uint32_t unit = kNone;
		if (!playerUnitPointer->resolve(&unitField) || !readSafe(unitField, unit)) return kNone;
		return unit;
	}

	// player 0's own datum (its record's salt, index 0): some damage is owned by the player rather than its body
	uint32_t playerDatum()
	{
		if (!playerUnitPointer) return kNone;
		uintptr_t unitField = 0;
		uint16_t salt = 0;
		if (!playerUnitPointer->resolve(&unitField) || unitField < kPlayerUnitField || !readSafe(unitField - kPlayerUnitField, salt) || salt == 0 || salt == 0xFFFF)
			return kNone;
		return (uint32_t)salt << 16;
	}

	std::string tagPathOf(uint32_t tag)
	{
		auto tagName = tagNameWeak.lock();
		if (!tagName || tag == kNone) return "";
		try
		{
			const char* name = tagName->getTagName(Datum((uint16_t)(tag & 0xFFFF), (uint16_t)(tag >> 16)));
			return name ? std::string(name) : std::string();
		}
		catch (HCMRuntimeException) { return ""; }
	}

	// every damage effect the rounds of this gun deal right now (after any element swap): impact and detonation
	const std::set<uint32_t>& damageOfGun(uint32_t tag)
	{
		if (auto it = mGunDamageCache.find(tag); it != mGunDamageCache.end()) return it->second;
		std::set<uint32_t>& out = mGunDamageCache[tag];
		auto tagTable = tagTableWeak.lock();
		auto tagBlocks = tagBlockWeak.lock();
		if (!tagTable || !tagBlocks) return out;
		static const int32_t weapGroup = MagicString(std::string("weap")).getNum();
		static const int32_t projGroup = MagicString(std::string("proj")).getNum();
		static const int32_t jptGroup = MagicString(std::string("jpt!")).getNum();
		auto weapon = tagTable->getTagByDatum(Datum((uint16_t)(tag & 0xFFFF), (uint16_t)(tag >> 16)));
		if (!weapon || weapon.value().tagType.getNum() != weapGroup) return out;
		auto barrels = block(*tagBlocks, weapon.value().tagAddress + kBarrels);
		if (!barrels) return out;
		for (size_t i = 0; i < barrels->elementCount && i < 8; i++)
		{
			uintptr_t b = barrels->firstElement + i * kBarrelSize;
			int32_t group = 0;
			uint32_t projectile = kNone;
			if (!readSafe(b + kProjectileRef, group) || group != projGroup || !readSafe(b + kProjectileDatum, projectile) || projectile == kNone) continue;
			auto proj = projectileTag(projectile);
			if (!proj) continue;
			for (size_t ref : { kProjImpactDamage, kProjDetonationDamage })
			{
				int32_t refGroup = 0;
				uint32_t datum = kNone;
				if (readSafe(*proj + ref, refGroup) && refGroup == jptGroup && readSafe(*proj + ref + kTagRefDatum, datum) && datum != kNone) out.insert(datum);
			}
		}
		return out;
	}

	uint32_t heldTag()
	{
		uint32_t unit = playerUnit();
		uintptr_t body = objectAddress(unit);
		int8_t slot = -1;
		if (!body || !readSafe(body + kUnitWeaponSlot, slot) || slot < 0 || slot > 3) return kNone;
		uint32_t weapon = kNone, parent = kNone, tag = kNone;
		if (!readSafe(body + kUnitWeapons + 4 * slot, weapon)) return kNone;
		uintptr_t gun = objectAddress(weapon, kWeaponObjectType);
		if (!gun || !readSafe(gun + kObjectParent, parent) || parent != unit || !readSafe(gun, tag)) return kNone;
		return tag;
	}

	// a biped just died: if we dealt the last hit with one of the guns we carry, that gun gets the kill
	void onDeath(uintptr_t body, uint32_t unit)
	{
		uint32_t owner[3] = { kNone, kNone, kNone }, effect = kNone;
		if (!readSafe(body + kObjectDamageOwner, owner) || !readSafe(body + kObjectLastDamageEffect, effect)) return;
		uint32_t player = playerDatum();
		bool ours = (unit != kNone && (owner[0] == unit || owner[1] == unit)) || (player != kNone && (owner[0] == player || owner[1] == player));
		// remembered for "mastery deaths" (developer): what the last few deaths looked like
		mRecentDeaths.push_back(std::format("{} died: owner {:08X} {:08X} {:08X}, effect {:08X} ({}); us: unit {:08X} player {:08X}{}",
			titleCase(lastSegment(tagPathOf(readTagOf(body)))), owner[0], owner[1], owner[2], effect, lastSegment(tagPathOf(effect)), unit, player, ours ? ", OURS" : ""));
		while (mRecentDeaths.size() > 8) mRecentDeaths.pop_front();
		if (!ours || effect == kNone) return;
		uintptr_t me = objectAddress(unit);
		int8_t slot = -1;
		uint32_t carried[4] = { kNone, kNone, kNone, kNone };
		if (!me || !readSafe(me + kUnitWeaponSlot, slot) || !readSafe(me + kUnitWeapons, carried)) return;
		std::vector<uint32_t> order;
		if (slot >= 0 && slot < 4) order.push_back(carried[slot]);	// the gun in our hands first
		for (int i = 0; i < 4; i++) if (i != slot) order.push_back(carried[i]);
		for (uint32_t weapon : order)
		{
			uintptr_t gun = objectAddress(weapon, kWeaponObjectType);
			uint32_t tag = kNone;
			if (!gun || !readSafe(gun, tag) || tag == kNone) continue;
			if (!damageOfGun(tag).contains(effect)) continue;
			credit(tag, 1);
			return;
		}
	}

	// look over every biped (a tenth of a second apart) for ones that have just died
	void scanKills()
	{
		if (!objectMetaDataTable || headerStride <= 0 || headerType + 1 > headerStride || headerPointer + 8 > headerStride) return;
		uintptr_t meta = 0;
		if (!objectMetaDataTable->resolve(&meta) || meta < 0x10000) return;
		size_t bytes = (size_t)headerStride * kObjectSlots;
		if (IsBadReadPtr((void*)meta, bytes)) return;
		mHeaderCopy.resize(bytes);
		memcpy(mHeaderCopy.data(), (void*)meta, bytes);
		uint32_t unit = playerUnit();
		std::unordered_map<uint32_t, bool> now;
		now.reserve(mDead.size() + 32);
		for (size_t i = 0; i < kObjectSlots; i++)
		{
			const uint8_t* h = mHeaderCopy.data() + headerStride * i;
			uint16_t salt = 0;
			uintptr_t address = 0;
			memcpy(&salt, h, sizeof(salt));
			memcpy(&address, h + headerPointer, sizeof(address));
			if (salt == 0 || address < 0x10000 || h[headerType] != kBipedObjectType) continue;
			uint32_t datum = ((uint32_t)salt << 16) | (uint32_t)i;
			uint32_t flags = 0;
			if (!readSafe(address + kObjectDamageFlags, flags)) continue;
			bool dead = (flags & kObjectDeadFlag) != 0;
			now[datum] = dead;
			if (!dead || !mScanPrimed || datum == unit) continue;
			auto it = mDead.find(datum);
			if (it == mDead.end() || it->second) continue;	// one we hadn't seen alive, or already dead
			onDeath(address, unit);
		}
		mDead.swap(now);
		mScanPrimed = true;
	}

	void credit(uint32_t tag, int kills)
	{
		std::string path = tagPathOf(tag);
		if (path.empty() || kills <= 0) return;
		auto key = masteryKey(path);
		int before = mKills[key];
		int after = std::min(before + kills, 1000000);
		mKills[key] = after;
		saveMastery();
		int levelBefore = masteryLevelFor(before), levelAfter = masteryLevelFor(after);
		mLastKill.tag = tag;
		mLastKill.name = titleCase(lastSegment(path));
		mLastKill.mastery = masteryOf(path);
		mLastKill.levelUp = levelAfter > levelBefore;
		mLastKill.atMs = nowMs();
		mHaveKill = true;
		for (auto* list : { &mWeapons, &mStandard })
			for (auto& w : *list)
				if (masteryKey(w.path) == key) w.mastery = mLastKill.mastery;
		if (levelAfter > levelBefore)
		{
			reapply();		// write the new level's bonuses
			mLevelUpTag = tag;
			mLevelUpAtMs = nowMs();
			std::string bonuses;
			for (auto& b : mLastKill.mastery.bonuses) bonuses += (bonuses.empty() ? "" : ", ") + b;
			mPendingMessage = std::format("{} mastery up! Level {} {}: {}.", mLastKill.name, levelAfter, kMasteryRanks[levelAfter], bonuses);
			PLOG_INFO << "loot: " << *mPendingMessage;
		}
	}

	void writeMastery(const Gun& gun, MasteryBonus b)
	{
		for (auto& br : gun.barrels)
		{
			patch<float>(br.address + kRoundsPerSecond, br.rps[0] * b.rate);
			patch<float>(br.address + kRoundsPerSecond + 4, br.rps[1] * b.rate);
			if (br.recovery > 0.f) patch<float>(br.address + kFireRecovery, br.recovery / b.rate);
			patch<float>(br.address + kMinimumError, br.minError * b.accuracy);
			patch<float>(br.address + kErrorAngle, br.error[0] * b.accuracy);
			patch<float>(br.address + kErrorAngle + 4, br.error[1] * b.accuracy);
		}
		if (gun.magazine && gun.loaded > 0)
		{
			int loaded = std::clamp((int)std::lround(gun.loaded * b.mag), (int)gun.loaded, 999);
			int total = std::clamp((int)std::lround(gun.total * b.mag), (int)gun.total, 9999);
			int initial = std::clamp((int)std::lround(gun.initial * b.mag), (int)gun.initial, total);
			patch<int16_t>(gun.magazine + kMagLoadedMax, (int16_t)loaded);
			patch<int16_t>(gun.magazine + kMagTotalMax, (int16_t)total);
			patch<int16_t>(gun.magazine + kMagTotalInitial, (int16_t)initial);
		}
	}

	// loot is off: every gun is standard issue plus whatever mastery it has earned
	int applyMastery()
	{
		undoPatches();
		mStandard.clear();
		std::map<uint32_t, std::string> projectilePaths;
		std::vector<Gun> guns;
		try { guns = findGuns(projectilePaths); }
		catch (HCMRuntimeException) { return 0; }
		int boosted = 0;
		for (auto& gun : guns)
		{
			if (!(gun.magazine && gun.loaded > 0)) mNoMagazine.insert(masteryKey(gun.path));
			LootWeapon card;
			card.tag = gun.tag;
			card.baseName = gun.name;
			card.path = gun.path;
			card.rarity = LootRarity::Common;
			card.maker = "Standard issue";
			card.rolled = false;
			card.mastery = masteryOf(gun.path);
			if (card.mastery.level > 0) { writeMastery(gun, masteryBonus(card.mastery.level)); boosted++; }
			mStandard.push_back(std::move(card));
		}
		mCardVersion++;
		if (boosted) PLOG_INFO << "loot: mastery bonuses on " << boosted << " guns";
		return boosted;
	}

	void reapply()
	{
		if (!mWeapons.empty()) roll(mSeed);
		else applyMastery();
	}

public:
	Impl(GameState game, IDIContainer& dicon) : mGame(game)
	{
		auto version = dicon.Resolve<IGetMCCVersion>().lock();
		if (game.operator GameState::Value() != GameState::Value::HaloReach || !version || version->getMCCVersionAsString() != "1.3528.0.0")
			throw HCMInitException("Loot only knows Halo: Reach's weapon layout (MCC 1.3528) so far");
		tagTableWeak = resolveDependentCheat(TagTableRange);
		tagNameWeak = resolveDependentCheat(GetTagName);
		tagBlockWeak = resolveDependentCheat(TagBlockReader);
		if (auto dirs = dicon.Resolve<DirPathContainer>().lock())
		{
			std::string folder = dirs->dirPath;
			if (!folder.empty() && folder.back() != '\\' && folder.back() != '/') folder += "\\";
			mMasteryPath = folder + "ModMenuMastery.txt";
			loadMastery();
		}
		mMasteryApplyAtMs = nowMs() + 3000;
		try
		{
			auto ptr = dicon.Resolve<PointerDataStore>().lock();
			playerUnitPointer = ptr->getData<std::shared_ptr<MultilevelPointer>>("playerDatum", game);
			objectMetaDataTable = ptr->getData<std::shared_ptr<MultilevelPointer>>("objectMetaDataTable", game);
			headerStride = *ptr->getData<std::shared_ptr<int64_t>>("objectHeaderStride", game);
			headerType = *ptr->getData<std::shared_ptr<int64_t>>("objectTypeOffset", game);
			headerPointer = *ptr->getData<std::shared_ptr<int64_t>>("objectOffsetOffset", game);
		}
		catch (HCMInitException ex)
		{
			PLOG_INFO << "loot: no object data, item cards are off: " << ex.what();
			playerUnitPointer.reset();
			objectMetaDataTable.reset();
		}
	}

	// the rolled gun the player is holding right now, or null
	const LootWeapon* heldWeapon()
	{
		if ((mWeapons.empty() && mStandard.empty()) || !playerUnitPointer || !objectMetaDataTable) return nullptr;
		uint32_t tag = heldTag();
		if (tag == kNone) return nullptr;
		for (auto& w : mWeapons) if (w.tag == tag) return &w;
		if (mWeapons.empty())
			for (auto& w : mStandard) if (w.tag == tag) return w.mastery.kills > 0 ? &w : nullptr;
		return nullptr;
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
		mProjectileNotes.clear();
		mFlightApplied.clear();
		std::vector<std::pair<size_t, std::set<uint32_t>>> firedBy;	// weapon index -> projectiles it fires after the roll

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

		// which damage effects each gun's own rounds use, and how many guns share each one: a gun only gets a damage
		// roll when its damage effect is its own (otherwise boosting it would boost another gun too)
		std::map<uint32_t, std::set<uint32_t>> damageUsers;	// damage effect -> guns
		std::map<uint32_t, std::vector<DamageEffect>> gunDamage;	// gun -> its damage effects
		for (auto& gun : guns)
			for (auto& b : gun.barrels)
				if (auto d = damageEffectOf(b.projectile))
				{
					damageUsers[d->datum].insert(gun.tag);
					auto& list = gunDamage[gun.tag];
					if (std::none_of(list.begin(), list.end(), [&](const DamageEffect& e) { return e.datum == d->datum; })) list.push_back(*d);
				}

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

			static const float dmgLo[] = { 1.0f, 1.05f, 1.15f, 1.25f, 1.4f }, dmgHi[] = { 1.1f, 1.25f, 1.4f, 1.6f, 2.0f };

			Roll roll;
			roll.rate = uniform(rateLo[r], rateHi[r]);
			roll.mag = uniform(magLo[r], magHi[r]);
			roll.damage = uniform(dmgLo[r], dmgHi[r]);

			// can this gun's damage be changed on its own? (its damage effects aren't shared with another gun)
			bool canDamage = false;
			if (auto it = gunDamage.find(gun.tag); it != gunDamage.end() && !it->second.empty())
				canDamage = std::all_of(it->second.begin(), it->second.end(), [&](const DamageEffect& e) { return damageUsers[e.datum].size() == 1; });

			// perks: 0 rapid, 1 deep mag, 2 twin, 3 scatter, 4 overcharged, 5 deadeye, 6 heavy (only when damage can change),
			// projectile perks 7 ricochet, 8 seeker, 9 railgun, 10 drifter, 11 mortar, 12 big bore, 13 fan
			std::set<int> perks;
			int wanted = perkCount[r] + (r == 2 && chance(0.5f) ? 1 : 0);
			if (wanted > 0 && chance(0.6f)) perks.insert(maker.favouredPerk);
			std::vector<int> allowed = { 0, 1, 2, 3, 5, 7, 8, 9, 10, 11, 12, 13 };
			if (canDamage) allowed.push_back(6);
			for (int tries = 0; (int)perks.size() < wanted && tries < 40; tries++) perks.insert(allowed[pick(allowed.size())]);
			if (rarity != LootRarity::Legendary) perks.erase(4); // overcharged is a legendary thing
			// one way of flying only: railgun, drifter and mortar fight over speed and gravity
			{
				bool flight = false;
				for (int f : { 9, 10, 11 }) { if (perks.contains(f)) { if (flight) perks.erase(f); flight = true; } }
			}

			// legendary specials: one signature trick each
			std::string special;
			if (rarity == LootRarity::Legendary)
			{
				std::vector<int> specials = { 3, 5 };
				if (gun.magazine && gun.loaded > 0) specials.push_back(0);		// endless only means something with a magazine
				if (elements.contains("Explosive")) { specials.push_back(1); specials.push_back(2); }
				if (canDamage) specials.push_back(4);
				switch (specials[pick(specials.size())])
				{
				case 0: roll.endless = true; special = "Special: Endless. Never needs reloading"; break;
				case 1: perks.insert(13); perks.erase(9); perks.erase(10); perks.erase(11); roll.ppsMul *= 5; roll.element = "Explosive"; special = "Special: Rocket Choir. A fan of explosive rounds"; break;
				case 2: perks.insert(7); roll.element = "Explosive"; special = "Special: Bounce House. Explosive rounds that bounce"; break;
				case 3: perks.insert(8); roll.ppsMul *= 3; roll.accuracy *= 1.6f; special = "Special: Swarm. Three seeking rounds per shot"; break;
				case 4: perks.erase(10); perks.erase(11); perks.insert(9); roll.damage *= 1.6f; special = "Special: Lancer. Hyper-velocity slugs that hit like a truck"; break;
				case 5: roll.rate *= 2.2f; roll.mag *= 3.f; special = "Special: Firehose. Double fire rate, triple magazine"; break;
				}
			}
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
				case 6: { float x = uniform(1.5f, 1.9f); roll.damage *= x; roll.rate *= 0.75f; roll.perks.push_back(std::format("Heavy: hits {:.0f}% harder, fires slower", (x - 1.f) * 100.f)); roll.adjective = "Heavy"; break; }
				case 7: roll.flight.insert(7); roll.adjective = "Ricochet"; break;
				case 8: roll.flight.insert(8); roll.adjective = "Seeking"; break;
				case 9: roll.flight.insert(9); roll.adjective = "Railgun"; break;
				case 10: roll.flight.insert(10); roll.adjective = "Drifting"; break;
				case 11: roll.flight.insert(11); roll.adjective = "Mortar"; break;
				case 12: roll.flight.insert(12); roll.adjective = "Big Bore"; break;
				case 13: roll.fan = true; if (roll.ppsMul == 1) roll.ppsMul = 3; roll.perks.push_back("Fan: rounds spray out in a flat line"); roll.adjective = "Fanning"; break;
				}
			}
			if (!special.empty()) roll.perks.insert(roll.perks.begin(), special);

			// element: swap in a projectile from another gun on this level (not for Heavy guns: they hit harder with their own)
			if (roll.element.empty() && chance(elementChance[r]) && !elements.empty() && !perks.contains(6))
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

			std::set<uint32_t> firedNow;	// the projectiles this gun fires after the roll
			int oldPps = gun.barrels.front().pps, newPps = oldPps;
			float oldRps = gun.barrels.front().rps[1];
			bool swapped = false;
			std::optional<uint32_t> elementProjectile;
			if (!roll.element.empty())
			{
				auto& pool = elements[roll.element];
				elementProjectile = pool[pick(pool.size())];
			}
			// mastery earned with this gun type goes on top of the roll (the card shows it in its own section)
			const bool hasMagazine = gun.magazine && gun.loaded > 0;
			if (!hasMagazine) mNoMagazine.insert(masteryKey(gun.path));
			loot.mastery = masteryOf(gun.path);
			const MasteryBonus mb = masteryBonus(loot.mastery.level);
			for (auto& b : gun.barrels)
			{
				float rate = std::min(roll.rate, 3.5f) * mb.rate;
				float rps[2] = { b.rps[0] * rate, b.rps[1] * rate };
				for (float& v : rps) v = std::min(v, std::max(50.f, std::max(b.rps[0], b.rps[1])));
				int pps = std::clamp(b.pps * roll.ppsMul, 1, 12);
				// keep the game sane: no more than ~120 projectiles a second out of one barrel
				while (pps > 1 && rps[1] * pps > 120.f) pps--;
				float accuracy = roll.accuracy * mb.accuracy;
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
				firedNow.insert(elementProjectile && *elementProjectile != b.projectile ? *elementProjectile : b.projectile);
				if (roll.fan)
				{
					int16_t distribution = -1;
					if (readSafe(b.address + kDistributionFunction, distribution) && distribution >= 0 && distribution <= 3)
					{
						patch<int16_t>(b.address + kDistributionFunction, 1);	// horizontal fan
						patch<float>(b.address + kDistributionAngle, 0.35f);		// about 20 degrees wide
					}
				}
				if (roll.endless)
				{
					int16_t used = -1;
					if (readSafe(b.address + kRoundsUsedPerShot, used) && used >= 0 && used <= 100) patch<int16_t>(b.address + kRoundsUsedPerShot, 0);
				}
				if (&b == &gun.barrels.front()) newPps = pps;
			}

			// projectile perks go on whatever this gun fires now; anything else firing the same rounds gets them too
			for (int f : roll.flight)
				for (uint32_t proj : firedNow)
				{
					if (mFlightApplied.contains({ proj, f })) continue;
					bool flies = f >= 9 && f <= 11;
					if (flies && (mFlightApplied.contains({ proj, 9 }) || mFlightApplied.contains({ proj, 10 }) || mFlightApplied.contains({ proj, 11 }))) continue;
					std::string line = applyFlight(proj, f);
					if (line.empty()) continue;
					mFlightApplied.insert({ proj, f });
					mProjectileNotes[proj].push_back(line);
				}
			if (roll.flight.contains(8) && gun.base)
			{
				float angle = 0.f, range = 0.f;
				if (readSafe(gun.base + kAutoaimAngle, angle) && readSafe(gun.base + kAutoaimRange, range) && sane(angle, 0.f, 1.5f) && sane(range, 0.f, 2000.f))
				{
					patch<float>(gun.base + kAutoaimAngle, std::max(angle, 0.12f));
					patch<float>(gun.base + kAutoaimRange, std::max(range, 30.f));
				}
			}
			int newLoaded = gun.loaded;
			if (gun.magazine && gun.loaded > 0)
			{
				float mag = std::min(roll.mag, 12.f) * mb.mag;
				newLoaded = std::clamp((int)std::lround(gun.loaded * mag), (int)gun.loaded, 999);
				int newTotal = std::clamp((int)std::lround(gun.total * mag), (int)gun.total, 9999);
				int newInitial = std::clamp((int)std::lround(gun.initial * mag), (int)gun.initial, newTotal);
				patch<int16_t>(gun.magazine + kMagLoadedMax, (int16_t)newLoaded);
				patch<int16_t>(gun.magazine + kMagTotalMax, (int16_t)newTotal);
				patch<int16_t>(gun.magazine + kMagTotalInitial, (int16_t)newInitial);
			}

			// damage: scale this gun's own damage effects, only when no other gun shares them and it still fires its own rounds
			float damage = std::min(roll.damage, 3.f);
			bool damaged = false;
			if (!swapped && canDamage && std::fabs(damage - 1.f) > 0.01f)
			{
				{
					for (auto& e : gunDamage[gun.tag])
					{
						float lower = 0.f, upper[2] = {};
						if (!readSafe(e.address + kDamageLower, lower) || !readSafe(e.address + kDamageUpper, upper)) continue;
						patch<float>(e.address + kDamageLower, lower * damage);
						patch<float>(e.address + kDamageUpper, upper[0] * damage);
						patch<float>(e.address + kDamageUpper + 4, upper[1] * damage);
						damaged = true;
					}
				}
			}

			loot.element = swapped ? roll.element : "";
			if (damaged) loot.stats.push_back({ "Damage", std::format("x{:.2f}", damage), damage >= 1.f });
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
			firedBy.push_back({ mWeapons.size() - 1, firedNow });
		}

		// projectile perks belong to the rounds: every gun firing them lists them (and can't claim ones that didn't stick)
		for (auto& [index, fired] : firedBy)
		{
			auto& perks = mWeapons[index].perks;
			for (uint32_t proj : fired)
			{
				auto it = mProjectileNotes.find(proj);
				if (it == mProjectileNotes.end()) continue;
				for (auto& note : it->second)
					if (std::find(perks.begin(), perks.end(), note) == perks.end()) perks.push_back(note);
			}
		}

		// best first, like a loot screen
		std::stable_sort(mWeapons.begin(), mWeapons.end(), [](const LootWeapon& a, const LootWeapon& b) { return (int)a.rarity > (int)b.rarity; });
		PLOG_INFO << "loot: rolled " << mWeapons.size() << " guns with seed " << mSeed << ", " << mPatches.size() << " values written";
		mStandard.clear();
		mCardVersion++;

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
		int boosted = applyMastery();
		std::string kept = boosted ? std::format(" Mastery bonuses stay on {} gun{}.", boosted, boosted == 1 ? "" : "s") : "";
		if (!had) return "Every gun is already standard issue." + kept;
		return std::format("Every gun is back to standard issue ({} values restored).{}", restored, kept);
	}

	std::optional<std::string> update(bool gamePlaying)
	{
		if (mLevelChanged.exchange(false))
		{
			// the level's tags were reloaded from disk: our values are gone, so forget them without writing anything
			mPatches.clear();
			mWeapons.clear();
			mStandard.clear();
			mGunDamageCache.clear();
			mDead.clear();
			mScanPrimed = false;
			mCardVersion++;
			mAutoRollAtMs = mAuto ? nowMs() + 4000 : 0;
			mMasteryApplyAtMs = mAuto ? 0 : nowMs() + 4000;
		}
		if (mAutoRollAtMs != 0 && gamePlaying && nowMs() >= mAutoRollAtMs)
		{
			mAutoRollAtMs = 0;
			auto message = roll(std::nullopt);
			if (mWeapons.empty()) { mAutoRollAtMs = nowMs() + 3000; return std::nullopt; } // still loading
			return "New loot for this mission. " + message;
		}
		// no loot this mission: still put the mastery bonuses on (and make the standard-issue cards)
		if (mMasteryApplyAtMs != 0 && gamePlaying && nowMs() >= mMasteryApplyAtMs)
		{
			mMasteryApplyAtMs = 0;
			if (mWeapons.empty())
			{
				applyMastery();
				if (mStandard.empty()) mMasteryApplyAtMs = nowMs() + 3000; // still loading
			}
		}
		// watch for kills
		if (gamePlaying && nowMs() >= mNextKillScanMs)
		{
			mNextKillScanMs = nowMs() + 100;
			try { scanKills(); }
			catch (HCMRuntimeException ex) { PLOG_ERROR << "loot: kill scan: " << ex.what(); }
		}
		if (mPendingMessage) return std::exchange(mPendingMessage, std::nullopt);
		return std::nullopt;
	}

	// ---- mastery, public
	const ModMenuLoot::MasteryKill* lastKill() const { return mHaveKill ? &mLastKill : nullptr; }

	std::vector<MasteryEntry> masteryList()
	{
		std::vector<MasteryEntry> out;
		for (auto& [path, kills] : mKills)
			if (kills > 0) out.push_back({ titleCase(lastSegment(path)), masteryOf(path) });
		std::stable_sort(out.begin(), out.end(), [](const MasteryEntry& a, const MasteryEntry& b) { return a.mastery.kills > b.mastery.kills; });
		return out;
	}

	std::string addMastery(int kills)
	{
		uint32_t tag = heldTag();
		if (tag == kNone) return "Hold a gun first.";
		credit(tag, std::clamp(kills, 1, 1000));
		auto m = masteryOf(tagPathOf(tag));
		return std::format("{}: {} kills, level {} {}", titleCase(lastSegment(tagPathOf(tag))), m.kills, m.level, m.rank);
	}

	std::string resetMastery(bool all)
	{
		std::string what;
		if (all) { mKills.clear(); what = "every gun"; }
		else
		{
			uint32_t tag = heldTag();
			if (tag == kNone) return "Hold a gun first (or reset all).";
			mKills.erase(masteryKey(tagPathOf(tag)));
			what = titleCase(lastSegment(tagPathOf(tag)));
		}
		saveMastery();
		mHaveKill = false;
		reapply();
		return "Mastery reset for " + what + ".";
	}

	std::vector<std::string> recentDeaths() const { return { mRecentDeaths.begin(), mRecentDeaths.end() }; }

	bool cardsAvailable() const
	{
		return !mWeapons.empty() || std::any_of(mStandard.begin(), mStandard.end(), [](const LootWeapon& w) { return w.mastery.kills > 0; });
	}
	uint32_t cardVersion() const { return mCardVersion; }
	bool levelUpCard(uint32_t tag) const { return tag != kNone && tag == mLevelUpTag && nowMs() - mLevelUpAtMs < 7000; }

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
const LootWeapon* ModMenuLoot::heldWeapon() { return pimpl->heldWeapon(); }
const ModMenuLoot::MasteryKill* ModMenuLoot::lastKill() const { return pimpl->lastKill(); }
std::vector<MasteryEntry> ModMenuLoot::masteryList() { return pimpl->masteryList(); }
std::string ModMenuLoot::addMastery(int kills) { return pimpl->addMastery(kills); }
std::string ModMenuLoot::resetMastery(bool all) { return pimpl->resetMastery(all); }
std::vector<std::string> ModMenuLoot::recentDeaths() const { return pimpl->recentDeaths(); }
bool ModMenuLoot::cardsAvailable() const { return pimpl->cardsAvailable(); }
uint32_t ModMenuLoot::cardVersion() const { return pimpl->cardVersion(); }
bool ModMenuLoot::levelUpCard(uint32_t tag) const { return pimpl->levelUpCard(tag); }
