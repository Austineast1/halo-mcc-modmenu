#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// In-game Mod Menu. Opens on the modMenuHotkey (default Insert), frees the cursor and blocks game input
// while open, and re-applies persistent mods after checkpoint reverts and level loads.
// Built on the same pattern as CommandConsoleManager.

class ModMenuManager : public IOptionalCheat
{
private:
	class ModMenuManagerImpl;
	std::unique_ptr<ModMenuManagerImpl> pimpl;

public:
	ModMenuManager(GameState gameImpl, IDIContainer& dicon);
	~ModMenuManager();

	virtual std::string_view getName() override {
		return nameof(ModMenuManager);
	}
};
