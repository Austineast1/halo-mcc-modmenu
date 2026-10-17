#include "pch.h"
#include "ModMenuFonts.h"
#include "ModMenuFontsData.h"

namespace
{
	ImFont* gDisplay = nullptr;
	ImFont* gBody = nullptr;
	ImFont* gBodyBold = nullptr;

	ImFont* addEmbedded(ImFontAtlas* atlas, const unsigned char* data, int size, float px, int oversample)
	{
		ImFontConfig cfg;
		cfg.FontDataOwnedByAtlas = false; // the arrays live in the DLL's read-only data
		cfg.OversampleH = oversample;
		cfg.OversampleV = 1;
		cfg.PixelSnapH = false;
		static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2013, 0x2026, 0x2715, 0x2726, 0 }; // Latin-1, dashes, quotes, bullet, ellipsis
		return atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), size, px, &cfg, ranges);
	}
}

namespace ModMenuFonts
{
	ImFont* addDefaultUIFont(ImFontAtlas* atlas, float px)
	{
		return addEmbedded(atlas, kModMenuFontManropeRegular, (int)sizeof(kModMenuFontManropeRegular), px, 3);
	}

	void addMenuFonts(ImFontAtlas* atlas)
	{
		gDisplay = addEmbedded(atlas, kModMenuFontCinzelSemiBold, (int)sizeof(kModMenuFontCinzelSemiBold), displayAtlasPx, 2);
		gBody = addEmbedded(atlas, kModMenuFontManropeRegular, (int)sizeof(kModMenuFontManropeRegular), bodyAtlasPx, 2);
		gBodyBold = addEmbedded(atlas, kModMenuFontManropeSemiBold, (int)sizeof(kModMenuFontManropeSemiBold), bodyAtlasPx, 2);
	}

	ImFont* display() { return gDisplay; }
	ImFont* body() { return gBody; }
	ImFont* bodyBold() { return gBodyBold; }

	Scoped::Scoped(ImFont* font, float px)
		: mFont(font ? font : ImGui::GetFont())
	{
		mPreviousScale = mFont->Scale;
		mFont->Scale = px / mFont->FontSize;
		ImGui::PushFont(mFont);
	}

	Scoped::~Scoped()
	{
		// restore the scale BEFORE popping, so the font we fall back to is measured at its own size
		mFont->Scale = mPreviousScale;
		ImGui::PopFont();
	}
}
