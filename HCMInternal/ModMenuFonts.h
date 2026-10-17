#pragma once
#include "imgui.h"

// Typefaces for the Mod Menu and HCM's main window.
// Cinzel SemiBold (engraved roman capitals) for titles, Manrope for everything else.
// Both are SIL Open Font License 1.1 and embedded in the DLL (see HCMInternal/fonts).
namespace ModMenuFonts
{
	// Size each font is rasterised at. Menus scale these down, so they stay crisp up to 4K.
	constexpr float displayAtlasPx = 72.f;
	constexpr float bodyAtlasPx = 40.f;

	// Manrope at the same pixel size as ImGui's old default font (13px), so HCM's hand-tuned
	// row heights still line up. Add it FIRST so it becomes the default font.
	ImFont* addDefaultUIFont(ImFontAtlas* atlas, float px = 13.f);

	// Adds the large display/body fonts the Mod Menu uses. Call before the atlas is built.
	void addMenuFonts(ImFontAtlas* atlas);

	// nullptr until addMenuFonts has run; callers fall back to ImGui::GetFont().
	ImFont* display();
	ImFont* body();
	ImFont* bodyBold();

	// Pushes a font at an exact pixel size and puts everything back on destruction.
	// Nesting the same font at different sizes is fine.
	class Scoped
	{
		ImFont* mFont;
		float mPreviousScale;
	public:
		Scoped(ImFont* font, float px);
		~Scoped();
		Scoped(const Scoped&) = delete;
		Scoped& operator=(const Scoped&) = delete;
	};
}
