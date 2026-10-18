#pragma once
#include "pch.h"
#include "ModuleCache.h"
#include <intrin.h>

// Halo: Reach (MCC 1.3528) keeps its live game state (object table, players) in the game thread's thread-local storage.
// In campaign, HCM's static pointers mirror it, but in Firefight they stay null, so the mod menu reads it straight from
// the game thread's TLS block instead (found statically in haloreach.dll: every object/player lookup does
// gs:[0x58] -> [_tls_index] -> block):
//   _tls_index at haloreach.dll +0xC17B18 (checked through the instruction that loads it, +0x49DBEC)
//   block +0x10 = object data array, block +0x18 = player data array
//   data array: element size +0x20 (qword), elements +0x50, each element starts with its salt (u16)
//   object header element (0x18): salt +0x0, type +0x4, object address +0x10; player element: unit datum +0x28
// capture() has to run on the game thread (a GameTickEvent callback); everything else reads what it saw.
namespace ReachGameThread
{
	inline std::atomic<uintptr_t> gBlock{ 0 };
	inline std::atomic<int> gChecked{ 0 };	// 0 unknown, 1 layout matches, -1 doesn't
	inline uintptr_t gModule = 0;

	constexpr uintptr_t kTlsIndex = 0xC17B18;
	constexpr uintptr_t kTlsIndexUse = 0x49DBEC;	// mov edx, [rip+0x779F26] (= _tls_index)
	inline const uint8_t kTlsIndexUseBytes[] = { 0x8B, 0x15, 0x26, 0x9F, 0x77, 0x00 };
	constexpr size_t kBlockObjects = 0x10, kBlockPlayers = 0x18;
	constexpr size_t kArrayElementSize = 0x20, kArrayElements = 0x50;
	constexpr size_t kHeaderType = 0x4, kHeaderObject = 0x10, kPlayerUnit = 0x28;
	constexpr uint32_t kNone = 0xFFFFFFFF;

	template <typename T> inline bool read(uintptr_t address, T& out)
	{
		if (address < 0x10000 || IsBadReadPtr((void*)address, sizeof(T))) return false;
		memcpy(&out, (void*)address, sizeof(T));
		return true;
	}

	inline bool layoutOk()
	{
		int checked = gChecked.load();
		if (checked != 0) return checked > 0;
		auto module = ModuleCache::getModuleHandle(L"haloreach.dll");
		bool ok = false;
		if (module)
		{
			gModule = (uintptr_t)module.value();
			auto at = (void*)(gModule + kTlsIndexUse);
			ok = !IsBadReadPtr(at, sizeof(kTlsIndexUseBytes)) && memcmp(at, kTlsIndexUseBytes, sizeof(kTlsIndexUseBytes)) == 0;
		}
		gChecked = ok ? 1 : -1;
		PLOG_INFO << "ReachGameThread: TLS layout " << (ok ? "matches" : "doesn't match") << " this haloreach.dll";
		return ok;
	}

	// game thread only
	inline void capture()
	{
		if (!layoutOk()) return;
		uint32_t index = 0;
		if (!read(gModule + kTlsIndex, index) || index > 4096) return;
		uintptr_t tlsArray = (uintptr_t)__readgsqword(0x58);
		uintptr_t block = 0;
		if (!read(tlsArray + (uintptr_t)index * 8, block) || !block) return;
		gBlock = block;
	}

	// a new level (or game) loads: capture again on the next tick
	inline void forget() { gBlock = 0; gChecked = 0; }

	inline bool dataArray(size_t which, uintptr_t& elements, uint64_t& stride)
	{
		uintptr_t block = gBlock.load(), array = 0;
		if (!block || !read(block + which, array) || !array) return false;
		if (!read(array + kArrayElements, elements) || !elements || !read(array + kArrayElementSize, stride) || stride == 0 || stride > 0x10000) return false;
		return true;
	}

	// the object header table (0x18 per object, 2048 objects)
	inline bool objectHeaders(uintptr_t& headers, uint64_t& stride) { return dataArray(kBlockObjects, headers, stride); }

	inline uintptr_t objectAddress(uint32_t datum)
	{
		if (datum == kNone) return 0;
		uintptr_t headers = 0;
		uint64_t stride = 0;
		uint16_t index = (uint16_t)(datum & 0xFFFF), salt = (uint16_t)(datum >> 16), headerSalt = 0;
		if (index >= 2048 || !objectHeaders(headers, stride)) return 0;
		uintptr_t header = headers + stride * index, address = 0;
		if (!read(header, headerSalt) || headerSalt != salt || !read(header + kHeaderObject, address)) return 0;
		return address;
	}

	inline uint32_t playerDatum(int index = 0)
	{
		uintptr_t players = 0;
		uint64_t stride = 0;
		uint16_t salt = 0;
		if (!dataArray(kBlockPlayers, players, stride) || !read(players + stride * index, salt) || salt == 0) return kNone;
		return ((uint32_t)salt << 16) | (uint32_t)index;
	}

	inline uint32_t playerUnit(int index = 0)
	{
		uintptr_t players = 0;
		uint64_t stride = 0;
		uint16_t salt = 0;
		uint32_t unit = kNone;
		if (!dataArray(kBlockPlayers, players, stride) || !read(players + stride * index, salt) || salt == 0) return kNone;
		if (!read(players + stride * index + kPlayerUnit, unit)) return kNone;
		return unit;
	}
}
