#include "pch.h"
#include "ModMenuGUI.h"
#include "SkullEnum.h"
#include "ModMenuFonts.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <shellapi.h>
#include <format>
#include <sstream>
#include <deque>
#include <array>
#include <cfloat>

namespace
{
	constexpr ImVec4 rgba(int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }

	// Palette: black lacquer, champagne gold and ivory.
	constexpr ImVec4 colWindow = rgba(11, 10, 8, 0.96f);
	constexpr ImVec4 colInk = rgba(12, 11, 9);				// solid, for text and knobs sitting on gold
	constexpr ImVec4 colPopup = rgba(19, 17, 14, 0.985f);
	constexpr ImVec4 colCard = rgba(255, 244, 222, 0.030f);
	constexpr ImVec4 colCardHover = rgba(255, 244, 222, 0.052f);
	constexpr ImVec4 colGold = rgba(212, 175, 106);
	constexpr ImVec4 colGoldBright = rgba(243, 220, 170);
	constexpr ImVec4 colGoldDeep = rgba(150, 120, 68);
	constexpr ImVec4 colIvory = rgba(238, 231, 216);
	constexpr ImVec4 colTaupe = rgba(165, 154, 136);
	constexpr ImVec4 colMuted = rgba(114, 106, 94);
	constexpr ImVec4 colFaint = rgba(74, 69, 61);
	constexpr ImVec4 colTrack = rgba(255, 244, 222, 0.10f);
	constexpr ImVec4 colField = rgba(255, 244, 222, 0.045f);
	constexpr ImVec4 colDanger = rgba(208, 108, 92);
	constexpr ImVec4 colClear = rgba(0, 0, 0, 0.f);

	ImVec4 withAlpha(ImVec4 c, float a) { c.w *= a; return c; }
	ImU32 col32(ImVec4 c, float alphaMul = 1.f) { c.w *= alphaMul; return ImGui::ColorConvertFloat4ToU32(c); }
	ImVec4 mix(ImVec4 a, ImVec4 b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
	ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

	std::string timestamp()
	{
		using namespace std::chrono;
		auto now = system_clock::now();
		auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
		std::time_t t = system_clock::to_time_t(now);
		std::tm local{};
		localtime_s(&local, &t);
		return std::format("{:02}:{:02}:{:02}.{:03}", local.tm_hour, local.tm_min, local.tm_sec, (int)ms.count());
	}

	std::string lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return s;
	}

	std::string upper(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
		return s;
	}

	// "ThatsJustWrong" -> "Thats Just Wrong", "IWHBYD" stays as is
	std::string spacedName(std::string_view s)
	{
		std::string out;
		for (size_t i = 0; i < s.size(); i++)
		{
			if (i > 0 && std::isupper((unsigned char)s[i]) && std::islower((unsigned char)s[i - 1])) out += ' ';
			out += s[i];
		}
		return out;
	}

	std::string_view visibleLabel(const char* label)
	{
		std::string_view v(label);
		auto hash = v.find("##");
		return hash == std::string_view::npos ? v : v.substr(0, hash);
	}

	// ---------------------------------------------------------------- type

	ImFont* fontOr(ImFont* f) { return f ? f : ImGui::GetFont(); }
	ImFont* fDisplay() { return fontOr(ModMenuFonts::display()); }
	ImFont* fBody() { return fontOr(ModMenuFonts::body()); }
	ImFont* fBold() { return fontOr(ModMenuFonts::bodyBold()); }

	// letter-spaced text (engraved titles and small-caps labels)
	float trackedWidth(ImFont* font, float px, std::string_view text, float tracking)
	{
		float w = 0.f;
		for (size_t i = 0; i < text.size(); i++)
		{
			w += font->CalcTextSizeA(px, FLT_MAX, 0.f, text.data() + i, text.data() + i + 1).x;
			if (i + 1 < text.size()) w += tracking;
		}
		return w;
	}

	void drawTracked(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, ImU32 col, std::string_view text, float tracking)
	{
		for (size_t i = 0; i < text.size(); i++)
		{
			const char* c = text.data() + i;
			dl->AddText(font, px, pos, col, c, c + 1);
			pos.x += font->CalcTextSizeA(px, FLT_MAX, 0.f, c, c + 1).x + tracking;
		}
	}

	void trackedItem(ImFont* font, float px, ImVec4 col, std::string_view text, float tracking)
	{
		ImVec2 p = ImGui::GetCursorScreenPos();
		float w = trackedWidth(font, px, text, tracking);
		drawTracked(ImGui::GetWindowDrawList(), font, px, p, col32(col), text, tracking);
		ImGui::Dummy(ImVec2(w, px));
	}

	void textItem(ImFont* font, float px, ImVec4 col, const std::string& s, float wrapWidth = 0.f)
	{
		ModMenuFonts::Scoped f(font, px);
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		if (wrapWidth > 0.f) ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
		ImGui::TextUnformatted(s.c_str());
		if (wrapWidth > 0.f) ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}

	// thin gold rule that fades out at both ends
	void goldRule(float width, float alpha = 0.55f)
	{
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		float mid = p.x + width * 0.5f;
		ImU32 clear = col32(colGold, 0.f), gold = col32(colGold, alpha);
		dl->AddRectFilledMultiColor(p, ImVec2(mid, p.y + 1.f), clear, gold, gold, clear);
		dl->AddRectFilledMultiColor(ImVec2(mid, p.y), ImVec2(p.x + width, p.y + 1.f), gold, clear, clear, gold);
		ImGui::Dummy(ImVec2(width, 1.f));
	}

	void diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
	{
		dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col);
	}

	// ---------------------------------------------------------------- motion

	// eased per-widget value kept in ImGui's state storage, so switches slide and hovers fade
	float animate(ImGuiID id, float target, float speed = 14.f)
	{
		ImGuiStorage* st = ImGui::GetStateStorage();
		float v = st->GetFloat(id, target);
		v += (target - v) * std::min(1.f, ImGui::GetIO().DeltaTime * speed);
		if (std::fabs(target - v) < 0.002f) v = target;
		st->SetFloat(id, v);
		return v;
	}

	// ---------------------------------------------------------------- widgets
	// Every size comes from S, the scale of the body text (1.0 = 20px body text).

	ImVec2 switchSize(float S) { return ImVec2(50.f * S, 28.f * S); }

	bool luxSwitch(const char* id, bool on, float S)
	{
		ImVec2 size = switchSize(S);
		ImVec2 p = ImGui::GetCursorScreenPos();
		bool pressed = ImGui::InvisibleButton(id, size);
		bool hovered = ImGui::IsItemHovered();
		float t = animate(ImGui::GetItemID(), on ? 1.f : 0.f);
		float h = animate(ImGui::GetItemID() + 1, hovered ? 1.f : 0.f, 18.f);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		float r = size.y * 0.5f;
		ImVec2 q = add(p, size);
		dl->AddRectFilled(p, q, col32(mix(colTrack, colGold, t)), r);
		dl->AddRect(p, q, col32(colGoldBright, 0.10f + 0.25f * t + 0.4f * h), r, 0, 1.f);

		float knobR = r - 3.5f * S;
		ImVec2 c(p.x + r + t * (size.x - 2.f * r), p.y + r);
		dl->AddCircleFilled(add(c, ImVec2(0.f, 1.5f * S)), knobR, col32(rgba(0, 0, 0, 0.35f)), 32);
		dl->AddCircleFilled(c, knobR, col32(mix(colIvory, colInk, t)), 32);
		return pressed;
	}

	enum class Btn { Primary, Outline, Ghost };

	float buttonTextPx(float B) { return B * 0.68f; }
	ImVec2 buttonPad(Btn style, float S) { return ImVec2(style == Btn::Ghost ? 9.f * S : 20.f * S, 10.f * S); }
	float buttonHeight(float B, float S) { return buttonTextPx(B) + buttonPad(Btn::Outline, S).y * 2.f; }
	float buttonWidth(const char* label, Btn style, float B, float S)
	{
		return trackedWidth(fBold(), buttonTextPx(B), visibleLabel(label), 1.6f * S) + buttonPad(style, S).x * 2.f;
	}

	bool luxButton(const char* label, Btn style, float B, float S, bool enabled = true)
	{
		ImFont* font = fBold();
		float px = buttonTextPx(B), tracking = 1.6f * S;
		auto text = visibleLabel(label);
		ImVec2 pad = buttonPad(style, S);
		ImVec2 size(trackedWidth(font, px, text, tracking) + pad.x * 2.f, px + pad.y * 2.f);
		ImVec2 p = ImGui::GetCursorScreenPos();

		if (!enabled) ImGui::BeginDisabled();
		bool pressed = ImGui::InvisibleButton(label, size);
		if (!enabled) ImGui::EndDisabled();
		bool held = ImGui::IsItemActive();
		float h = animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.f : 0.f, 18.f);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 q = add(p, size);
		float r = size.y * 0.5f;
		ImVec4 textCol;
		switch (style)
		{
		case Btn::Primary:
			dl->AddRectFilled(p, q, col32(held ? colGoldDeep : mix(colGold, colGoldBright, h)), r);
			textCol = colInk;
			break;
		case Btn::Outline:
			dl->AddRectFilled(p, q, col32(colGold, 0.13f * h + (held ? 0.12f : 0.f)), r);
			dl->AddRect(p, q, col32(colGold, 0.5f + 0.5f * h), r, 0, 1.f);
			textCol = mix(colGold, colGoldBright, h);
			break;
		case Btn::Ghost:
			textCol = mix(colTaupe, colGoldBright, h);
			break;
		}
		if (!enabled) textCol = colFaint;
		drawTracked(dl, font, px, add(p, pad), col32(textCol), text, tracking);
		return pressed && enabled;
	}

	// Slim gold slider. Returns true while the value changes; `released` is set the frame the drag ends.
	bool luxSlider(const char* id, float* v, float mn, float mx, float step, float width, float S, bool* released)
	{
		float h = 28.f * S;
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton(id, ImVec2(std::max(width, 40.f * S), h));
		bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
		float knobR = 9.f * S;
		float x0 = p.x + knobR, x1 = p.x + std::max(width, 40.f * S) - knobR;

		bool changed = false;
		if (active && x1 > x0 && mx > mn)
		{
			float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.f, 1.f);
			float nv = mn + t * (mx - mn);
			if (step > 0.f) nv = mn + std::round((nv - mn) / step) * step;
			nv = std::clamp(nv, mn, mx);
			if (nv != *v) { *v = nv; changed = true; }
		}
		*released = ImGui::IsItemDeactivated();

		float t = mx > mn ? std::clamp((*v - mn) / (mx - mn), 0.f, 1.f) : 0.f;
		float cy = p.y + h * 0.5f, kx = x0 + t * (x1 - x0);
		float g = animate(ImGui::GetItemID(), (hovered || active) ? 1.f : 0.f, 16.f);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(ImVec2(x0, cy - 1.5f * S), ImVec2(x1, cy + 1.5f * S), col32(colTrack), 2.f * S);
		dl->AddRectFilled(ImVec2(x0, cy - 1.5f * S), ImVec2(kx, cy + 1.5f * S), col32(colGold), 2.f * S);
		if (g > 0.01f) dl->AddCircleFilled(ImVec2(kx, cy), knobR * (1.f + 0.9f * g), col32(colGold, 0.14f * g), 40);
		dl->AddCircleFilled(ImVec2(kx, cy + 1.5f * S), knobR, col32(rgba(0, 0, 0, 0.4f)), 32);
		dl->AddCircleFilled(ImVec2(kx, cy), knobR, col32(colIvory), 32);
		dl->AddCircle(ImVec2(kx, cy), knobR, col32(colGold), 32, 2.f * S);
		return changed;
	}

	float tagHeight(float B, float S) { return B * 0.52f + 7.f * S; }

	void tagPill(const std::string& tag, float B, float S)
	{
		std::string t = upper(tag);
		ImFont* font = fBold();
		float px = B * 0.52f, tracking = 1.4f * S;
		ImVec2 pad(9.f * S, 3.5f * S);
		ImVec2 size(trackedWidth(font, px, t, tracking) + pad.x * 2.f, px + pad.y * 2.f);
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::Dummy(size);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRect(p, add(p, size), col32(colGold, 0.55f), size.y * 0.5f, 0, 1.f);
		drawTracked(dl, font, px, add(p, pad), col32(colGold), t, tracking);
	}

	// Puts the next item on the same row, vertically centred in a row of height rowH that starts at rowTop.
	void rowNext(float rowTop, float rowH, float itemH, float spacing)
	{
		ImGui::SameLine(0.f, spacing);
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(p.x, rowTop + (rowH - itemH) * 0.5f));
	}

	// ---------------------------------------------------------------- cards
	// Card background is drawn after the content (on a lower draw channel) so it can wrap whatever is inside.

	struct Card
	{
		ImVec2 start;
		float width, padX, padY;
		float innerWidth() const { return width - padX * 2.f; }
	};

	Card beginCard(float S)
	{
		Card c;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		c.start = ImGui::GetCursorScreenPos();
		c.width = ImGui::GetContentRegionAvail().x;
		c.padX = 24.f * S;
		c.padY = 18.f * S;
		ImGui::SetCursorScreenPos(add(c.start, ImVec2(c.padX, c.padY)));
		ImGui::BeginGroup();
		return c;
	}

	void endCard(const Card& c, bool active, float S)
	{
		ImGui::EndGroup();
		ImVec2 end(c.start.x + c.width, ImGui::GetItemRectMax().y + c.padY);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->ChannelsSetCurrent(0);
		bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(c.start, end);
		float r = 14.f * S;
		float a = animate(ImGui::GetID("##cardactive"), active ? 1.f : 0.f, 10.f);
		ImVec4 bg = mix(hovered ? colCardHover : colCard, withAlpha(colGold, 0.07f), a);
		dl->AddRectFilled(c.start, end, col32(bg), r);
		dl->AddRect(c.start, end, col32(colGold, 0.07f + (hovered ? 0.13f : 0.f) + 0.33f * a), r, 0, 1.f);
		if (a > 0.01f) // gold inlay down the left edge of anything that's switched on
			dl->AddRectFilled(ImVec2(c.start.x + 1.f, c.start.y + r), ImVec2(c.start.x + 1.f + 2.5f * S, end.y - r), col32(colGold, a), 2.f * S);
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(ImVec2(c.start.x, end.y));
		ImGui::Dummy(ImVec2(c.width, 4.f * S));
	}

	const std::map<SkullEnum, const char*>& skullDescriptions()
	{
		static const std::map<SkullEnum, const char*> d =
		{
			{SkullEnum::Anger, "Enemies fire faster and more often."},
			{SkullEnum::Assassins, "Every enemy is permanently cloaked."},
			{SkullEnum::BlackEye, "Shields only recharge when you melee enemies."},
			{SkullEnum::Blind, "No HUD and no weapon on screen."},
			{SkullEnum::Catch, "Enemies throw and drop far more grenades."},
			{SkullEnum::EyePatch, "No aim assist."},
			{SkullEnum::Famine, "Dropped weapons carry half ammo."},
			{SkullEnum::Fog, "No motion tracker."},
			{SkullEnum::Foreign, "You can't use the other side's weapons."},
			{SkullEnum::Iron, "Dying sends you back to the start or your last checkpoint."},
			{SkullEnum::Jacked, "Ground vehicles can only be entered by boarding."},
			{SkullEnum::Masterblaster, "Co-op role swap between overshield melee and infinite ammo."},
			{SkullEnum::Mythic, "Enemies have more health."},
			{SkullEnum::Recession, "Every shot costs double ammo."},
			{SkullEnum::SoAngry, "Enraged Brutes explode if you don't kill them fast."},
			{SkullEnum::Streaking, "Shields drain over time but refill on kills."},
			{SkullEnum::Swarm, "Hunters are much stronger."},
			{SkullEnum::ThatsJustWrong, "Enemies notice you faster."},
			{SkullEnum::TheyComeBack, "Reanimated Flood are far more dangerous."},
			{SkullEnum::Thunderstorm, "Most enemies are promoted a rank."},
			{SkullEnum::Tilt, "Enemy resistances and weaknesses are stronger."},
			{SkullEnum::ToughLuck, "Enemies always dodge, always go berserk, never flee."},
			{SkullEnum::Bandanna, "Infinite ammo, grenades and abilities."},
			{SkullEnum::BondedPair, "Co-op: when your partner dies you get a damage boost."},
			{SkullEnum::Boom, "Explosion radius doubled."},
			{SkullEnum::Cowbell, "Explosions throw things much harder."},
			{SkullEnum::Envy, "Flashlight swapped for active camo."},
			{SkullEnum::Feather, "Melee hits send things flying."},
			{SkullEnum::Ghost, "AI never flinch."},
			{SkullEnum::GruntBirthdayParty, "Grunt headshots pop confetti."},
			{SkullEnum::GruntFuneral, "Grunts explode in plasma when they die."},
			{SkullEnum::IWHBYD, "Rare combat dialogue is common."},
			{SkullEnum::Malfunction, "A random part of your HUD breaks on respawn."},
			{SkullEnum::Pinata, "Punched enemies drop grenades."},
			{SkullEnum::ProphetBirthdayParty, "Prophet surprise."},
			{SkullEnum::Scarab, "Every weapon you hold fires a Scarab beam."},
			{SkullEnum::Sputnik, "Objects are lighter and fly further."},
			{SkullEnum::Acrophobia, "Infinite jetpack flight."},
		};
		return d;
	}
}

ModMenuGUI::ModMenuGUI(GameState game, std::shared_ptr<GameEngineFunctions> engine, std::weak_ptr<SettingsStateAndEvents> settings, std::string dirPath)
	: mGame(game), mEngine(engine), mSettingsWeak(settings)
{
	if (!dirPath.empty() && dirPath.back() != '\\' && dirPath.back() != '/') dirPath += "\\";
	mLibraryPath = dirPath + "ModMenuLibrary.xml";
	loadLibrary();
}

// ------------------------------------------------------------------ library

void ModMenuGUI::loadLibrary()
{
	std::string builtInText = ModLibrary::defaultXML();
	auto builtIn = ModLibrary::parse(builtInText);
	if (!builtIn)
	{
		mLibrary = ModLibrary{};
		mLibraryStatus = "Built-in library failed to load: " + builtIn.error();
		mLibraryStatusIsError = true;
		addLog(mLibraryStatus, LogLine::Kind::Error);
		return;
	}

	auto writeBuiltIn = [&]() -> bool
		{
			try
			{
				std::ofstream out(mLibraryPath, std::ios::binary | std::ios::trunc);
				out << builtInText;
				return out.good();
			}
			catch (...) { return false; }
		};

	mLibraryStatusIsError = false;

	if (std::filesystem::exists(mLibraryPath))
	{
		std::ifstream in(mLibraryPath, std::ios::binary);
		std::stringstream buffer;
		buffer << in.rdbuf();
		auto fromFile = ModLibrary::parse(buffer.str());

		if (!fromFile)
		{
			mLibrary = builtIn.value();
			mLibraryStatus = std::format("ModMenuLibrary.xml has an error ({}). Using the built-in library.", fromFile.error());
			mLibraryStatusIsError = true;
		}
		else if (fromFile->version < builtIn->version)
		{
			// ship newer built-in mods, but keep the user's old file
			auto backup = mLibraryPath.substr(0, mLibraryPath.size() - 4) + std::format(".backup-v{}.xml", fromFile->version);
			try { std::filesystem::copy_file(mLibraryPath, backup, std::filesystem::copy_options::overwrite_existing); }
			catch (...) {}
			writeBuiltIn();
			mLibrary = builtIn.value();
			mLibraryStatus = std::format("Library updated to v{}. Your old file was saved next to it.", builtIn->version);
		}
		else
		{
			mLibrary = fromFile.value();
			mLibraryStatus = std::format("Loaded ModMenuLibrary.xml v{}", mLibrary.version);
		}
	}
	else
	{
		mLibrary = builtIn.value();
		mLibraryStatus = writeBuiltIn()
			? std::format("Created ModMenuLibrary.xml v{} next to HCM. Edit it to add your own mods.", mLibrary.version)
			: std::string("Couldn't write ModMenuLibrary.xml, using the built-in library.");
	}

	// set up runtime values for any mods we haven't seen yet
	for (auto& cat : mLibrary.categories)
	{
		for (auto& mod : cat.mods)
		{
			if (mState.contains(mod.id)) continue;
			auto& rt = mState[mod.id];
			rt.value = std::clamp(mod.defaultValue, mod.minValue, mod.maxValue);
			if (mod.type == ModType::HCMSlider)
			{
				if (auto s = hcmDouble(mod.hcmKey)) rt.value = (float)s->GetValue();
			}
		}
	}

	if (mSelectedCategory >= (int)mLibrary.categories.size()) mSelectedCategory = 0;
	addLog(mLibraryStatus, mLibraryStatusIsError ? LogLine::Kind::Error : LogLine::Kind::Info);
}

// ------------------------------------------------------------------ logging

void ModMenuGUI::addLog(const std::string& text, LogLine::Kind kind)
{
	std::lock_guard<std::mutex> lock(mLogMutex);
	mLog.push_back(LogLine{ timestamp(), text, kind });
	while (mLog.size() > mMaxLog) mLog.pop_front();
	mScrollLogToBottom = true;
}

void ModMenuGUI::onHaloScriptOutput(const std::string& outputString, const HSOutputType& outputType)
{
	addLog(outputString, outputType == HSOutputType::Error ? LogLine::Kind::Error : LogLine::Kind::Output);
	if (outputType == HSOutputType::Error)
		toast("The game didn't accept that one here: " + outputString, true);
}

// ------------------------------------------------------------------ execution

// Splits "(ai_allegiance a b)" / "(ai_allegiance_remove a b)" into its parts. Returns false for anything else.
static bool parseAllegiance(const std::string& command, bool& isRemove, std::pair<std::string, std::string>& teams)
{
	std::istringstream in(command);
	std::string head, a, b;
	if (!(in >> head >> a >> b)) return false;
	if (head == "(ai_allegiance") isRemove = false;
	else if (head == "(ai_allegiance_remove") isRemove = true;
	else return false;
	if (!b.empty() && b.back() == ')') b.pop_back();
	if (a.empty() || b.empty()) return false;
	teams = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
	return true;
}

void ModMenuGUI::sendLines(const std::vector<std::string>& lines, const std::string& value)
{
	if (mForgetAlliances.exchange(false)) { mAlliancesWeMade.clear(); mPlayerTeamChanged = false; }
	for (auto& line : lines)
	{
		auto command = ModMenuText::expand(line, mGame, value);

		// Only ever break an alliance this menu made. Removing one the game or a Workshop mod set up
		// (or one that doesn't exist) is asking for trouble.
		bool isRemove = false;
		std::pair<std::string, std::string> teams;
		if (parseAllegiance(command, isRemove, teams))
		{
			if (isRemove)
			{
				if (!mAlliancesWeMade.contains(teams))
				{
					addLog(std::format("skipped {} (the menu didn't make that alliance)", command), LogLine::Kind::Info);
					continue;
				}
				mAlliancesWeMade.erase(teams);
			}
			else
			{
				mAlliancesWeMade.insert(teams);
			}
		}

		// Same idea for the player's own team: only put it back to "player" if this menu moved it.
		// In mods where you start on another team (playing as an Elite, say) the reset would turn your allies on you.
		if (command.starts_with("(object_set_allegiance "))
		{
			bool backToPlayer = command.ends_with(" player)");
			if (backToPlayer && !mPlayerTeamChanged)
			{
				addLog(std::format("skipped {} (the menu didn't change your team)", command), LogLine::Kind::Info);
				continue;
			}
			mPlayerTeamChanged = !backToPlayer;
		}

		addLog(command, LogLine::Kind::Command);
		mCommandsSent++;
		try
		{
			mEngine->SendCommand(command.c_str());
		}
		catch (HCMRuntimeException ex)
		{
			addLog(std::format("HCM error: {}", ex.what()), LogLine::Kind::Error);
			toast("Couldn't reach the game engine for that one.", true);
		}
	}
}

ModMenuGUI::ModRuntime& ModMenuGUI::runtime(const ModDef& mod)
{
	return mState[mod.id];
}

void ModMenuGUI::applyToggle(const ModDef& mod, bool on)
{
	auto cmds = mod.commandsFor(mGame);
	if (!cmds) return;
	auto& rt = runtime(mod);

	if (on)
	{
		if (!cmds->on.empty()) sendLines(cmds->on, "true");
		else sendLines(cmds->set, "true");
	}
	else
	{
		if (!cmds->off.empty()) sendLines(cmds->off, "false");
		else sendLines(cmds->set, "false");
	}

	bool canTurnOff = !cmds->off.empty() || !cmds->set.empty();
	rt.on = canTurnOff ? on : false;
}

void ModMenuGUI::applySlider(const ModDef& mod, float value)
{
	auto cmds = mod.commandsFor(mGame);
	if (!cmds) return;
	auto& rt = runtime(mod);
	rt.value = std::clamp(value, mod.minValue, mod.maxValue);
	sendLines(cmds->apply, ModMenuText::formatNumber(rt.value));
	bool hasDefault = mod.defaultValue >= mod.minValue && mod.defaultValue <= mod.maxValue;
	rt.applied = !hasDefault || std::fabs(rt.value - mod.defaultValue) > mod.step * 0.5f;
}

void ModMenuGUI::applyChoice(const ModDef& mod, int optionIndex)
{
	auto cmds = mod.commandsFor(mGame);
	if (!cmds || optionIndex < 0 || optionIndex >= (int)mod.options.size()) return;
	auto& rt = runtime(mod);
	if (rt.applied && rt.option != optionIndex && !cmds->off.empty())
		sendLines(cmds->off, mod.options[rt.option].value); // undo the previous pick first
	rt.option = optionIndex;
	sendLines(cmds->apply, mod.options[optionIndex].value);
	rt.applied = true;
}

void ModMenuGUI::undoChoice(const ModDef& mod)
{
	auto cmds = mod.commandsFor(mGame);
	auto& rt = runtime(mod);
	if (!cmds || !rt.applied) return;
	if (!cmds->off.empty()) sendLines(cmds->off, mod.options[rt.option].value);
	rt.applied = false;
}

void ModMenuGUI::runButton(const ModDef& mod)
{
	auto cmds = mod.commandsFor(mGame);
	if (!cmds) return;
	sendLines(cmds->run, "");

	// one-at-a-time groups (Become forms): the newest one replaces the rest, and is remembered
	// so it can be put back after a death or checkpoint revert if it's marked reapply
	if (!mod.group.empty())
	{
		for (auto& cat : mLibrary.categories)
			for (auto& other : cat.mods)
				if (other.group == mod.group) runtime(other).on = false;
	}
	if (mod.reapply) runtime(mod).on = true;
}

void ModMenuGUI::runText(const ModDef& mod, const std::string& text)
{
	auto cmds = mod.commandsFor(mGame);
	if (!cmds || text.empty()) return;
	sendLines(cmds->apply, text);
}

int ModMenuGUI::reapplyActive(const std::string& reason)
{
	int count = 0;
	for (auto& cat : mLibrary.categories)
	{
		for (auto& mod : cat.mods)
		{
			if (!mod.reapply || !mod.supports(mGame)) continue;
			auto& rt = runtime(mod);
			auto cmds = mod.commandsFor(mGame);
			if (!cmds) continue;

			switch (mod.type)
			{
			case ModType::Toggle:
				if (rt.on) { applyToggle(mod, true); count++; }
				break;
			case ModType::Slider:
				if (rt.applied) { applySlider(mod, rt.value); count++; }
				break;
			case ModType::Choice:
				if (rt.applied) { sendLines(cmds->apply, mod.options[rt.option].value); count++; }
				break;
			case ModType::Button:
				if (rt.on) { sendLines(cmds->run, ""); count++; }
				break;
			default:
				break;
			}
		}
	}
	if (count > 0) addLog(std::format("Re-applied {} mod{} after {}", count, count == 1 ? "" : "s", reason), LogLine::Kind::Info);
	return count;
}

bool ModMenuGUI::hasActiveReapplyMods()
{
	for (auto& cat : mLibrary.categories)
		for (auto& mod : cat.mods)
			if (mod.reapply && mod.supports(mGame) && isActive(mod)) return true;
	return false;
}

void ModMenuGUI::turnEverythingOff()
{
	// remembered one-at-a-time forms: forget them and run their group's reset button once (e.g. Back to Spartan)
	std::set<std::string> groupsToReset;
	for (auto& cat : mLibrary.categories)
		for (auto& mod : cat.mods)
			if (mod.type == ModType::Button && mod.supports(mGame) && runtime(mod).on)
			{
				runtime(mod).on = false;
				if (!mod.group.empty()) groupsToReset.insert(mod.group);
			}
	for (auto& cat : mLibrary.categories)
		for (auto& mod : cat.mods)
			if (mod.type == ModType::Button && !mod.reapply && groupsToReset.contains(mod.group) && mod.supports(mGame))
			{
				if (auto cmds = mod.commandsFor(mGame)) sendLines(cmds->run, "");
				groupsToReset.erase(mod.group);
			}

	for (auto& cat : mLibrary.categories)
	{
		for (auto& mod : cat.mods)
		{
			if (!mod.supports(mGame)) continue;
			auto& rt = runtime(mod);
			switch (mod.type)
			{
			case ModType::Toggle:
				if (rt.on) applyToggle(mod, false);
				break;
			case ModType::Slider:
				if (rt.applied)
				{
					if (mod.defaultValue >= mod.minValue && mod.defaultValue <= mod.maxValue) applySlider(mod, mod.defaultValue);
					else rt.applied = false;
				}
				break;
			case ModType::Choice:
				if (rt.applied) undoChoice(mod);
				break;
			case ModType::HCMToggle:
				if (hcmBoolValue(mod)) setHcmBool(mod, false);
				break;
			default:
				break;
			}
		}
	}
	addLog("Turned every mod off", LogLine::Kind::Info);
}

// ------------------------------------------------------------------ HCM bridges

std::shared_ptr<BinarySetting<bool>> ModMenuGUI::hcmBool(const std::string& key)
{
	auto s = mSettingsWeak.lock();
	if (!s) return nullptr;
	if (key == "invulnerability") return s->invulnerabilityToggle;
	if (key == "invulnerabilityNPC") return s->invulnerabilityNPCToggle;
	if (key == "aiFreeze") return s->aiFreezeToggle;
	if (key == "medusa") return s->medusaToggle;
	if (key == "infiniteAmmo") return s->infiniteAmmoToggle;
	if (key == "bottomlessClip") return s->bottomlessClipToggle;
	if (key == "speedhack") return s->speedhackToggle;
	if (key == "disableBarriers") return s->disableBarriersToggle;
	if (key == "hideHUD") return s->hideHUDToggle;
	if (key == "freeCamera") return s->freeCameraToggle;
	if (key == "disableFog") return s->disableFogToggle;
	if (key == "naturalCheckpointDisable") return s->naturalCheckpointDisable;
	if (key == "display2DInfo") return s->display2DInfoToggle;
	if (key == "togglePause") return s->togglePause;
	return nullptr;
}

std::shared_ptr<BinarySetting<double>> ModMenuGUI::hcmDouble(const std::string& key)
{
	auto s = mSettingsWeak.lock();
	if (!s) return nullptr;
	if (key == "speedhackValue") return s->speedhackSetting;
	return nullptr;
}

std::shared_ptr<ActionEvent> ModMenuGUI::hcmAction(const std::string& key)
{
	auto s = mSettingsWeak.lock();
	if (!s) return nullptr;
	if (key == "forceCheckpoint") return s->forceCheckpointEvent;
	if (key == "forceRevert") return s->forceRevertEvent;
	if (key == "forceDoubleRevert") return s->forceDoubleRevertEvent;
	if (key == "forceMissionRestart") return s->forceMissionRestartEvent;
	if (key == "forceTeleport") return s->forceTeleportEvent;
	if (key == "forceLaunch") return s->forceLaunchEvent;
	if (key == "setPlayerHealth") return s->setPlayerHealthEvent;
	if (key == "injectCheckpoint") return s->injectCheckpointEvent;
	if (key == "dumpCheckpoint") return s->dumpCheckpointEvent;
	return nullptr;
}

bool ModMenuGUI::hcmBoolValue(const ModDef& mod)
{
	auto setting = hcmBool(mod.hcmKey);
	return setting ? setting->GetValue() : false;
}

void ModMenuGUI::setHcmBool(const ModDef& mod, bool value)
{
	auto setting = hcmBool(mod.hcmKey);
	if (!setting)
	{
		addLog(std::format("Unknown HCM toggle '{}'", mod.hcmKey), LogLine::Kind::Error);
		return;
	}
	setting->GetValueDisplay() = value;
	setting->UpdateValueWithInput();
	addLog(std::format("HCM {} = {}", mod.hcmKey, value ? "on" : "off"), LogLine::Kind::Info);
}

// ------------------------------------------------------------------ skulls

std::map<SkullEnum, bool> ModMenuGUI::readSkulls()
{
	std::map<SkullEnum, bool> out;
	auto s = mSettingsWeak.lock();
	if (!s) return out;

	try
	{
		s->updateSkullBitBoolCollectionEvent->operator()(mGame);
		ScopedAtomicBool lock(s->skullBitBoolCollectionInUse);
		for (auto& [skull, pointer] : s->skullBitBoolCollection)
			out[skull] = pointer.operator const bool();
	}
	catch (HCMRuntimeException ex)
	{
		addLog(std::format("Skull read failed: {}", ex.what()), LogLine::Kind::Error);
	}
	return out;
}

void ModMenuGUI::flipSkull(SkullEnum skull)
{
	auto s = mSettingsWeak.lock();
	if (!s) return;
	try
	{
		ScopedAtomicBool lock(s->skullBitBoolCollectionInUse);
		auto it = s->skullBitBoolCollection.find(skull);
		if (it == s->skullBitBoolCollection.end()) return;
		bool previous = it->second.operator const bool();
		it->second.set(!previous);
		addLog(std::format("Skull {} {}", magic_enum::enum_name(skull), previous ? "off" : "on"), LogLine::Kind::Info);
	}
	catch (HCMRuntimeException ex)
	{
		addLog(std::format("Skull toggle failed: {}", ex.what()), LogLine::Kind::Error);
	}
}

// ------------------------------------------------------------------ state helpers

bool ModMenuGUI::isActive(const ModDef& mod)
{
	auto& rt = runtime(mod);
	switch (mod.type)
	{
	case ModType::Toggle: return rt.on;
	case ModType::Slider: return rt.applied;
	case ModType::Choice: return rt.applied;
	case ModType::HCMToggle: return hcmBoolValue(mod);
	case ModType::Button: return mod.reapply && rt.on; // a remembered Become form
	default: return false;
	}
}

int ModMenuGUI::activeCount()
{
	int n = 0;
	for (auto& cat : mLibrary.categories)
		for (auto& mod : cat.mods)
			if (mod.supports(mGame) && isActive(mod)) n++;
	return n;
}

bool ModMenuGUI::matchesSearch(const ModDef& mod) const
{
	if (mSearch.empty()) return true;
	auto q = lower(mSearch);
	return lower(mod.name).contains(q) || lower(mod.desc).contains(q) || lower(mod.id).contains(q) || lower(mod.tag).contains(q);
}

std::vector<const ModDef*> ModMenuGUI::visibleMods(const ModCategory& cat)
{
	std::vector<const ModDef*> out;
	std::map<SkullEnum, bool> skulls;
	bool skullsRead = false;

	for (auto& mod : cat.mods)
	{
		if (!mod.supports(mGame) || !matchesSearch(mod)) continue;

		if (mod.type == ModType::Skull)
		{
			if (!skullsRead) { skulls = readSkulls(); skullsRead = true; }
			auto skull = magic_enum::enum_cast<SkullEnum>(mod.skullKey);
			if (!skull || !skulls.contains(skull.value())) continue; // this game doesn't have it
		}
		out.push_back(&mod);
	}
	return out;
}

// ------------------------------------------------------------------ rendering

void ModMenuGUI::toast(const std::string& text, bool isError)
{
	std::lock_guard<std::mutex> lock(mLogMutex);
	mToastText = text;
	mToastIsError = isError;
	mToastAt = std::chrono::steady_clock::now();
}

namespace
{
	constexpr int kStyleColorCount = 26;
	constexpr int kStyleVarCount = 15;
}

void ModMenuGUI::pushStyle()
{
	const float S = mBasePx / 20.f;
	ImGui::PushStyleColor(ImGuiCol_WindowBg, colWindow);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, colClear);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, colPopup);
	ImGui::PushStyleColor(ImGuiCol_Border, withAlpha(colGold, 0.22f));
	ImGui::PushStyleColor(ImGuiCol_BorderShadow, colClear);
	ImGui::PushStyleColor(ImGuiCol_Text, colIvory);
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, colMuted);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, colField);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, withAlpha(colIvory, 0.07f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, withAlpha(colGold, 0.10f));
	ImGui::PushStyleColor(ImGuiCol_Button, colClear);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, withAlpha(colGold, 0.12f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, withAlpha(colGold, 0.20f));
	ImGui::PushStyleColor(ImGuiCol_Header, withAlpha(colGold, 0.12f));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, withAlpha(colGold, 0.16f));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, withAlpha(colGold, 0.22f));
	ImGui::PushStyleColor(ImGuiCol_CheckMark, colGold);
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, colGold);
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, colGoldBright);
	ImGui::PushStyleColor(ImGuiCol_Separator, withAlpha(colGold, 0.18f));
	ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, colClear);
	ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, withAlpha(colGold, 0.20f));
	ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, withAlpha(colGold, 0.38f));
	ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, withAlpha(colGold, 0.55f));
	ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, withAlpha(colGold, 0.30f));
	ImGui::PushStyleColor(ImGuiCol_ResizeGrip, colClear);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.f * S);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.f * S);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f * S);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 14.f * S);
	ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.f * S);
	ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 6.f * S);
	ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.f * S);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36.f * S, 28.f * S));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f * S, 9.f * S));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.f * S, 8.f * S));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(10.f * S, 6.f * S));
}

void ModMenuGUI::popStyle()
{
	ImGui::PopStyleVar(kStyleVarCount);
	ImGui::PopStyleColor(kStyleColorCount);
}

void ModMenuGUI::renderBackdrop()
{
	const float S = mBasePx / 20.f;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();

	// a soft warm light falling from the top edge
	ImGui::PushClipRect(wp, add(wp, ws), false);
	ImVec2 c(wp.x + ws.x * 0.5f, wp.y);
	float maxR = std::min(ws.x * 0.46f, ws.y * 0.9f);
	constexpr int layers = 26;
	for (int i = 0; i < layers; i++)
	{
		float r = maxR * (1.f - (float)i / layers);
		dl->AddCircleFilled(c, r, col32(colGold, 0.0048f), 96);
	}
	ImGui::PopClipRect();

	// fine gold corner brackets
	float inset = 12.f * S, len = 26.f * S;
	ImU32 gold = col32(colGold, 0.55f);
	ImVec2 tl(wp.x + inset, wp.y + inset), br(wp.x + ws.x - inset, wp.y + ws.y - inset);
	dl->AddLine(ImVec2(tl.x, tl.y + len), tl, gold); dl->AddLine(tl, ImVec2(tl.x + len, tl.y), gold);
	dl->AddLine(ImVec2(br.x - len, tl.y), ImVec2(br.x, tl.y), gold); dl->AddLine(ImVec2(br.x, tl.y), ImVec2(br.x, tl.y + len), gold);
	dl->AddLine(ImVec2(tl.x, br.y - len), ImVec2(tl.x, br.y), gold); dl->AddLine(ImVec2(tl.x, br.y), ImVec2(tl.x + len, br.y), gold);
	dl->AddLine(ImVec2(br.x - len, br.y), br, gold); dl->AddLine(br, ImVec2(br.x, br.y - len), gold);
}

void ModMenuGUI::renderHeader()
{
	const float B = mBasePx, S = B / 20.f;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 top = ImGui::GetCursorScreenPos();
	float W = ImGui::GetContentRegionAvail().x;

	// eyebrow, then the engraved title
	trackedItem(fBold(), B * 0.62f, colGold, upper(ModMenuText::longGameName(mGame)), 3.2f * S);
	ImGui::Dummy(ImVec2(0.f, 1.f * S));
	trackedItem(fDisplay(), B * 2.0f, colIvory, upper(mLibrary.title.empty() ? std::string("Mod Menu") : mLibrary.title), 5.f * S);
	ImVec2 belowTitle = ImGui::GetCursorScreenPos();

	// close button, top right
	float d = 40.f * S;
	ImVec2 cp(top.x + W - d, top.y + 2.f * S);
	ImGui::SetCursorScreenPos(cp);
	if (ImGui::InvisibleButton("##modmenuclose", ImVec2(d, d))) mCloseRequested = true;
	float h = animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.f : 0.f, 18.f);
	ImVec2 cc(cp.x + d * 0.5f, cp.y + d * 0.5f);
	dl->AddCircleFilled(cc, d * 0.5f, col32(colGold, 0.12f * h), 48);
	dl->AddCircle(cc, d * 0.5f, col32(colGold, 0.35f + 0.5f * h), 48, 1.f);
	float x = 6.f * S;
	ImU32 xcol = col32(mix(colGold, colGoldBright, h));
	dl->AddLine(ImVec2(cc.x - x, cc.y - x), ImVec2(cc.x + x, cc.y + x), xcol, 1.6f * S);
	dl->AddLine(ImVec2(cc.x - x, cc.y + x), ImVec2(cc.x + x, cc.y - x), xcol, 1.6f * S);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close (Insert)");

	// key hint beside it
	std::string_view hint = "INSERT";
	float hpx = B * 0.52f, htr = 2.4f * S;
	float hw = trackedWidth(fBold(), hpx, hint, htr);
	drawTracked(dl, fBold(), hpx, ImVec2(cp.x - 16.f * S - hw, cc.y - hpx * 0.5f), col32(colTaupe, 0.8f), hint, htr);

	ImGui::SetCursorScreenPos(belowTitle);
	ImGui::Dummy(ImVec2(0.f, 6.f * S));
	goldRule(W, 0.6f);
	ImGui::Dummy(ImVec2(0.f, 12.f * S));

	// search pill with an active-count chip beside it
	int active = activeCount();
	std::string chip = active > 0 ? std::format("{} ACTIVE", active) : std::string("NONE ACTIVE");
	float chipPx = B * 0.56f, chipTr = 2.f * S;
	float chipW = trackedWidth(fBold(), chipPx, chip, chipTr) + 44.f * S;

	float searchPx = B * 0.9f;
	float fieldH = searchPx + 22.f * S;
	ImVec2 sp = ImGui::GetCursorScreenPos();
	{
		ModMenuFonts::Scoped f(ModMenuFonts::body(), searchPx);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, fieldH * 0.5f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(46.f * S, 11.f * S));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
		ImGui::PushStyleColor(ImGuiCol_Border, withAlpha(colGold, 0.22f));
		ImGui::SetNextItemWidth(W - chipW - 14.f * S);
		ImGui::InputTextWithHint("##modsearch", "Search mods", &mSearch);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
	}
	// magnifier
	float mr = 6.5f * S;
	ImVec2 mc(sp.x + 24.f * S, sp.y + fieldH * 0.5f - 1.f * S);
	dl->AddCircle(mc, mr, col32(colGold, 0.8f), 24, 1.6f * S);
	dl->AddLine(ImVec2(mc.x + mr * 0.7f, mc.y + mr * 0.7f), ImVec2(mc.x + mr * 1.5f, mc.y + mr * 1.5f), col32(colGold, 0.8f), 1.8f * S);

	// chip
	ImVec2 chipP(sp.x + W - chipW, sp.y);
	ImVec2 chipQ(chipP.x + chipW, sp.y + fieldH);
	dl->AddRect(chipP, chipQ, col32(colGold, active > 0 ? 0.6f : 0.18f), fieldH * 0.5f, 0, 1.f);
	if (active > 0) dl->AddRectFilled(chipP, chipQ, col32(colGold, 0.08f), fieldH * 0.5f);
	float dotX = chipP.x + 18.f * S;
	diamond(dl, ImVec2(dotX, sp.y + fieldH * 0.5f), 3.5f * S, col32(active > 0 ? colGold : colFaint));
	drawTracked(dl, fBold(), chipPx, ImVec2(dotX + 12.f * S, sp.y + (fieldH - chipPx) * 0.5f), col32(active > 0 ? colGold : colMuted), chip, chipTr);

	if (mGame.operator GameState::Value() == GameState::Value::Halo4)
	{
		ImGui::Dummy(ImVec2(0.f, 2.f * S));
		textItem(ModMenuFonts::body(), B * 0.72f, colMuted, "Halo 4 has no published script list, so a few scripted mods may do nothing here.");
	}
	ImGui::Dummy(ImVec2(0.f, 10.f * S));
}

void ModMenuGUI::renderCategoryList()
{
	const float B = mBasePx, S = B / 20.f;
	ImDrawList* dl = ImGui::GetWindowDrawList();

	trackedItem(fBold(), B * 0.54f, colMuted, "COLLECTIONS", 3.f * S);
	ImGui::Dummy(ImVec2(0.f, 6.f * S));

	for (int i = 0; i < (int)mLibrary.categories.size(); i++)
	{
		auto& cat = mLibrary.categories[i];
		int total = 0, active = 0;

		if (cat.special == "skulls")
		{
			if (!mSearch.empty()) continue;
			auto skulls = readSkulls();
			total = (int)skulls.size();
			for (auto& [k, v] : skulls) if (v) active++;
		}
		else if (cat.special == "spawn")
		{
			if (!mSearch.empty()) continue;
			auto* catalogue = mSpawnCatalogue ? mSpawnCatalogue() : nullptr;
			total = catalogue ? (int)catalogue->size() : 0;
		}
		else
		{
			for (auto* mod : visibleMods(cat))
			{
				total++;
				if (isActive(*mod)) active++;
			}
		}

		if (total == 0 && i != mSelectedCategory) continue; // collections with nothing for this game stay out of the way

		bool selected = mSelectedCategory == i && mSearch.empty();
		ImVec2 p = ImGui::GetCursorScreenPos();
		float w = ImGui::GetContentRegionAvail().x, h = 46.f * S;
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("##cat", ImVec2(w, h)))
		{
			mSelectedCategory = i;
			mSearch.clear();
		}
		float hv = animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.f : 0.f, 18.f);
		float sel = animate(ImGui::GetItemID() + 1, selected ? 1.f : 0.f, 14.f);
		ImGui::PopID();

		ImVec2 q(p.x + w, p.y + h);
		if (sel > 0.01f) dl->AddRectFilled(p, q, col32(colGold, 0.085f * sel), 11.f * S);
		else if (hv > 0.01f) dl->AddRectFilled(p, q, col32(colIvory, 0.03f * hv), 11.f * S);
		if (sel > 0.01f) dl->AddRectFilled(ImVec2(p.x, p.y + 13.f * S), ImVec2(p.x + 3.f * S, q.y - 13.f * S), col32(colGold, sel), 2.f * S);

		float px = B * 0.95f;
		ImVec4 textCol = total == 0 ? colFaint : mix(mix(colTaupe, colIvory, hv), colGoldBright, sel);
		dl->AddText(fBold(), px, ImVec2(p.x + 20.f * S, p.y + (h - px) * 0.5f), col32(textCol), cat.name.c_str());

		if (active > 0)
		{
			auto n = std::format("{}", active);
			float npx = B * 0.62f;
			float nw = fBold()->CalcTextSizeA(npx, FLT_MAX, 0.f, n.c_str()).x;
			float cx = q.x - 16.f * S - nw;
			dl->AddText(fBold(), npx, ImVec2(cx, p.y + (h - npx) * 0.5f), col32(colGold), n.c_str());
			diamond(dl, ImVec2(cx - 9.f * S, p.y + h * 0.5f), 3.f * S, col32(colGold));
		}
		ImGui::Dummy(ImVec2(0.f, 2.f * S));
	}
}

void ModMenuGUI::renderModList()
{
	const float B = mBasePx, S = B / 20.f;

	if (mLibrary.categories.empty())
	{
		textItem(ModMenuFonts::body(), B * 0.9f, colDanger, "No mods loaded. " + mLibraryStatus, ImGui::GetContentRegionAvail().x);
		return;
	}

	auto sectionTitle = [&](const std::string& name, float px)
		{
			trackedItem(fDisplay(), px, colGold, upper(name), 2.2f * S);
		};

	if (!mSearch.empty())
	{
		int found = 0;
		for (auto& cat : mLibrary.categories)
		{
			auto mods = visibleMods(cat);
			if (mods.empty()) continue;
			ImGui::Dummy(ImVec2(0.f, 4.f * S));
			sectionTitle(cat.name, B * 1.05f);
			ImGui::Dummy(ImVec2(0.f, 4.f * S));
			for (auto* mod : mods) { renderMod(*mod); found++; }
		}
		if (found == 0)
			textItem(ModMenuFonts::body(), B * 0.9f, colTaupe, std::format("Nothing called \"{}\" works in {}.", mSearch, ModMenuText::longGameName(mGame)));
		return;
	}

	auto& cat = mLibrary.categories[mSelectedCategory];
	sectionTitle(cat.name, B * 1.45f);
	ImGui::Dummy(ImVec2(0.f, 2.f * S));
	if (!cat.desc.empty()) textItem(ModMenuFonts::body(), B * 0.86f, colTaupe, cat.desc, ImGui::GetContentRegionAvail().x);
	ImGui::Dummy(ImVec2(0.f, 12.f * S));

	if (cat.special == "skulls")
	{
		renderSkullPanel();
		return;
	}
	if (cat.special == "spawn")
	{
		renderSpawnPanel();
		return;
	}

	auto mods = visibleMods(cat);
	if (mods.empty())
	{
		textItem(ModMenuFonts::body(), B * 0.9f, colTaupe, std::format("Nothing in this collection works in {}.", ModMenuText::longGameName(mGame)));
		textItem(ModMenuFonts::body(), B * 0.8f, colMuted, "Load a different game in MCC to use these.");
		return;
	}

	for (auto* mod : mods) renderMod(*mod);
}

void ModMenuGUI::renderMod(const ModDef& mod)
{
	const float B = mBasePx, S = B / 20.f;
	ImGui::PushID(mod.id.c_str());
	auto& rt = runtime(mod);
	auto cmds = mod.commandsFor(mGame);
	bool active = isActive(mod);
	bool canTurnOff = cmds && (!cmds->off.empty() || !cmds->set.empty());

	// what sits on the right of the card
	enum class Ctl { None, Switch, Action } ctl = Ctl::None;
	bool switchOn = false;
	std::optional<SkullEnum> skull;
	switch (mod.type)
	{
	case ModType::Toggle:
		if (canTurnOff) { ctl = Ctl::Switch; switchOn = rt.on; }
		else ctl = Ctl::Action;
		break;
	case ModType::HCMToggle:
		ctl = Ctl::Switch;
		switchOn = hcmBoolValue(mod);
		break;
	case ModType::Skull:
	{
		ctl = Ctl::Switch;
		auto s = magic_enum::enum_cast<SkullEnum>(mod.skullKey);
		if (s) skull = s.value();
		auto skulls = readSkulls();
		switchOn = skull && skulls.contains(*skull) && skulls.at(*skull);
		active = switchOn;
		break;
	}
	case ModType::Button:
	case ModType::HCMAction:
		ctl = Ctl::Action;
		break;
	default:
		break;
	}
	bool formOn = mod.type == ModType::Button && mod.reapply && rt.on;
	const char* actionLabel = formOn ? "ACTIVE" : "ACTIVATE";
	const Btn actionStyle = formOn ? Btn::Primary : Btn::Outline;

	Card card = beginCard(S);
	float innerW = card.innerWidth();
	ImVec2 controlSize(0.f, 0.f);
	if (ctl == Ctl::Switch) controlSize = switchSize(S);
	else if (ctl == Ctl::Action) controlSize = ImVec2(buttonWidth(actionLabel, actionStyle, B, S), buttonHeight(B, S));
	float textW = std::max(140.f * S, innerW - controlSize.x - (ctl == Ctl::None ? 0.f : 32.f * S));

	// left: name, tag, description
	ImVec2 top = ImGui::GetCursorScreenPos();
	float namePx = B * 1.04f;
	textItem(ModMenuFonts::bodyBold(), namePx, active ? colGoldBright : colIvory, mod.name);
	if (!mod.tag.empty())
	{
		rowNext(top.y, namePx, tagHeight(B, S), 12.f * S);
		tagPill(mod.tag, B, S);
	}
	if (!mod.desc.empty())
		textItem(ModMenuFonts::body(), B * 0.8f, colTaupe, mod.desc, textW);
	ImVec2 afterText = ImGui::GetCursorScreenPos();
	float textBottom = afterText.y - ImGui::GetStyle().ItemSpacing.y;
	bool hasRowControls = mod.type == ModType::Slider || mod.type == ModType::HCMSlider || mod.type == ModType::Choice || mod.type == ModType::Text;

	// right: switch or button, centred against the text
	if (ctl != Ctl::None)
	{
		float blockH = textBottom - top.y;
		ImGui::SetCursorScreenPos(ImVec2(top.x + innerW - controlSize.x, top.y + std::max(0.f, (blockH - controlSize.y) * 0.5f)));
		if (ctl == Ctl::Switch)
		{
			if (luxSwitch("##switch", switchOn, S))
			{
				bool want = !switchOn;
				if (mod.type == ModType::Toggle) applyToggle(mod, want);
				else if (mod.type == ModType::HCMToggle) setHcmBool(mod, want);
				else if (skull) flipSkull(*skull);
				toast(std::format("{} {}", mod.name, want ? "on" : "off"));
			}
		}
		else if (luxButton(actionLabel, actionStyle, B, S))
		{
			std::optional<std::string> message = std::format("{} activated", mod.name);
			if (mod.type == ModType::Toggle) applyToggle(mod, true);
			else if (mod.type == ModType::Button) runButton(mod);
			else if (mod.hcmKey == "commandConsole") mOpenConsoleRequested = true;
			else if (mod.hcmKey.starts_with("possess"))
			{
				message = mCustomAction ? mCustomAction(mod.hcmKey) : std::string("True possession isn't available here.");
				addLog(std::format("{}: {}", mod.hcmKey, *message), LogLine::Kind::Info);
			}
			else if (auto ev = hcmAction(mod.hcmKey))
			{
				addLog(std::format("HCM action {}", mod.hcmKey), LogLine::Kind::Info);
				ev->operator()();
			}
			else addLog(std::format("Unknown HCM action '{}'", mod.hcmKey), LogLine::Kind::Error);
			toast(*message);
		}
		// go back under the text (or to the top-left, which is already inside the card, if nothing follows)
		ImGui::SetCursorScreenPos(hasRowControls ? afterText : top);
	}

	// controls that need a full row
	const float rowH = std::max(28.f * S, buttonHeight(B, S));
	const float gap = 14.f * S;
	switch (mod.type)
	{
	case ModType::Slider:
	case ModType::HCMSlider:
	{
		ImGui::Dummy(ImVec2(0.f, 2.f * S));
		bool hasDefault = mod.defaultValue >= mod.minValue && mod.defaultValue <= mod.maxValue;
		float valuePx = B * 0.9f;
		std::string widest = ModMenuText::formatNumber(mod.maxValue);
		if (ModMenuText::formatNumber(mod.minValue).size() > widest.size()) widest = ModMenuText::formatNumber(mod.minValue);
		float valueW = fBold()->CalcTextSizeA(valuePx, FLT_MAX, 0.f, (widest + ".00").c_str()).x;
		float applyW = buttonWidth("APPLY", Btn::Outline, B, S);
		float resetW = buttonWidth("RESET", Btn::Ghost, B, S) + gap; // reserved even when hidden so every slider lines up
		float sliderW = std::max(80.f * S, innerW - valueW - applyW - resetW - gap * 2.f);

		float rowTop = ImGui::GetCursorScreenPos().y;
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowTop + (rowH - 28.f * S) * 0.5f));
		bool released = false;
		float v = rt.value;
		if (luxSlider("##slider", &v, mod.minValue, mod.maxValue, mod.step, sliderW, S, &released)) rt.value = v;
		bool commit = released;

		rowNext(rowTop, rowH, valuePx, gap);
		{
			ImVec2 vp = ImGui::GetCursorScreenPos();
			auto vs = ModMenuText::formatNumber(rt.value);
			ImGui::GetWindowDrawList()->AddText(fBold(), valuePx, vp, col32(rt.applied ? colGoldBright : colIvory), vs.c_str());
			ImGui::Dummy(ImVec2(valueW, valuePx));
		}
		rowNext(rowTop, rowH, buttonHeight(B, S), gap);
		if (luxButton("APPLY", Btn::Outline, B, S)) commit = true;
		if (hasDefault)
		{
			rowNext(rowTop, rowH, buttonHeight(B, S), gap);
			if (luxButton("RESET", Btn::Ghost, B, S)) { rt.value = mod.defaultValue; commit = true; }
		}

		if (commit)
		{
			if (mod.type == ModType::Slider)
			{
				applySlider(mod, rt.value);
			}
			else if (auto setting = hcmDouble(mod.hcmKey))
			{
				setting->GetValueDisplay() = (double)rt.value;
				setting->UpdateValueWithInput();
				addLog(std::format("HCM {} = {}", mod.hcmKey, ModMenuText::formatNumber(rt.value)), LogLine::Kind::Info);
			}
			toast(std::format("{} set to {}", mod.name, ModMenuText::formatNumber(rt.value)));
		}
		break;
	}
	case ModType::Choice:
	{
		std::vector<int> indices;
		for (int i = 0; i < (int)mod.options.size(); i++)
			if (mod.options[i].supports(mGame)) indices.push_back(i);
		if (indices.empty()) break;
		if (std::find(indices.begin(), indices.end(), rt.option) == indices.end()) rt.option = indices.front();

		ImGui::Dummy(ImVec2(0.f, 2.f * S));
		float applyW = buttonWidth("APPLY", Btn::Outline, B, S);
		float undoW = buttonWidth("UNDO", Btn::Ghost, B, S);
		float comboW = std::max(120.f * S, innerW - applyW - undoW - gap * 2.f);
		float comboPx = B * 0.86f;

		float rowTop = ImGui::GetCursorScreenPos().y;
		float comboH = comboPx + 16.f * S;
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowTop + (rowH - comboH) * 0.5f));
		{
			ModMenuFonts::Scoped f(ModMenuFonts::body(), comboPx);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, comboH * 0.5f);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18.f * S, 8.f * S));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f * S, 10.f * S));
			ImGui::PushStyleColor(ImGuiCol_Text, colIvory);
			ImGui::SetNextItemWidth(comboW);
			bool open = ImGui::BeginCombo("##choice", mod.options[rt.option].label.c_str(), ImGuiComboFlags_NoArrowButton);
			{
				ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
				float cx = b.x - 22.f * S, cy = (a.y + b.y) * 0.5f, k = 4.5f * S;
				ImGui::GetWindowDrawList()->AddPolyline(std::array<ImVec2, 3>{ ImVec2(cx - k, cy - k * 0.5f), ImVec2(cx, cy + k * 0.5f), ImVec2(cx + k, cy - k * 0.5f) }.data(), 3, col32(colGold), ImDrawFlags_None, 1.6f * S);
			}
			if (open)
			{
				for (int i : indices)
				{
					bool sel = i == rt.option;
					ImGui::PushStyleColor(ImGuiCol_Text, sel ? colGoldBright : colIvory);
					if (ImGui::Selectable(mod.options[i].label.c_str(), sel)) rt.option = i;
					ImGui::PopStyleColor();
					if (sel) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::PopStyleColor();
			ImGui::PopStyleVar(3);
		}
		rowNext(rowTop, rowH, buttonHeight(B, S), gap);
		if (luxButton("APPLY", Btn::Outline, B, S))
		{
			applyChoice(mod, rt.option);
			toast(std::format("{}: {}", mod.name, mod.options[rt.option].label));
		}
		rowNext(rowTop, rowH, buttonHeight(B, S), gap);
		if (luxButton("UNDO", Btn::Ghost, B, S, rt.applied))
		{
			undoChoice(mod);
			toast(std::format("{} undone", mod.name));
		}
		if (rt.applied)
		{
			ImGui::Dummy(ImVec2(0.f, 2.f * S));
			trackedItem(fBold(), B * 0.56f, colGold, upper("Active: " + mod.options[rt.option].label), 2.f * S);
		}
		break;
	}
	case ModType::Text:
	{
		ImGui::Dummy(ImVec2(0.f, 2.f * S));
		float runW = buttonWidth("RUN", Btn::Outline, B, S);
		float inputPx = B * 0.86f;
		float inputH = inputPx + 18.f * S;
		float rowTop = ImGui::GetCursorScreenPos().y;
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowTop + (rowH - inputH) * 0.5f));
		bool enter = false;
		{
			ModMenuFonts::Scoped f(ModMenuFonts::body(), inputPx);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, inputH * 0.5f);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18.f * S, 9.f * S));
			ImGui::SetNextItemWidth(std::max(120.f * S, innerW - runW - gap));
			enter = ImGui::InputTextWithHint("##text", "Type a command", &rt.text, ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::PopStyleVar(2);
		}
		rowNext(rowTop, rowH, buttonHeight(B, S), gap);
		if (luxButton("RUN", Btn::Outline, B, S) || enter)
		{
			runText(mod, rt.text);
			if (!rt.text.empty()) toast("Sent: " + rt.text);
		}
		break;
	}
	default:
		break;
	}

	endCard(card, active, S);
	ImGui::PopID();
}

void ModMenuGUI::renderSkullPanel()
{
	const float B = mBasePx, S = B / 20.f;
	auto skulls = readSkulls();
	if (skulls.empty())
	{
		textItem(ModMenuFonts::body(), B * 0.9f, colTaupe, "Skulls appear once you're in a mission.");
		return;
	}

	int on = 0;
	for (auto& [k, v] : skulls) if (v) on++;
	trackedItem(fBold(), B * 0.56f, on > 0 ? colGold : colMuted, std::format("{} OF {} ACTIVE  -  CHANGES APPLY INSTANTLY", on, (int)skulls.size()), 2.2f * S);
	ImGui::Dummy(ImVec2(0.f, 8.f * S));

	for (auto& [skull, value] : skulls)
	{
		ImGui::PushID((int)skull);
		Card card = beginCard(S);
		float innerW = card.innerWidth();
		ImVec2 sw = switchSize(S);
		float textW = std::max(140.f * S, innerW - sw.x - 32.f * S);

		ImVec2 top = ImGui::GetCursorScreenPos();
		textItem(ModMenuFonts::bodyBold(), B * 1.0f, value ? colGoldBright : colIvory, spacedName(magic_enum::enum_name(skull)));
		auto it = skullDescriptions().find(skull);
		if (it != skullDescriptions().end())
			textItem(ModMenuFonts::body(), B * 0.8f, colTaupe, it->second, textW);
		float blockH = ImGui::GetItemRectMax().y - top.y;
		ImVec2 after = ImGui::GetCursorScreenPos();

		ImGui::SetCursorScreenPos(ImVec2(top.x + innerW - sw.x, top.y + std::max(0.f, (blockH - sw.y) * 0.5f)));
		if (luxSwitch("##sk", value, S))
		{
			flipSkull(skull);
			toast(std::format("{} skull {}", spacedName(magic_enum::enum_name(skull)), value ? "off" : "on"));
		}
		ImGui::SetCursorScreenPos(after);
		endCard(card, value, S);
		ImGui::PopID();
	}
}

void ModMenuGUI::renderSpawnPanel()
{
	const float B = mBasePx, S = B / 20.f;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	auto* catalogue = mSpawnCatalogue ? mSpawnCatalogue() : nullptr;
	if (!catalogue)
	{
		textItem(ModMenuFonts::body(), B * 0.9f, colTaupe, "Spawning works in Halo: Reach for now. More games are on the way.");
		return;
	}
	if (catalogue->empty())
	{
		textItem(ModMenuFonts::body(), B * 0.9f, colTaupe, "Reading what this level has loaded... (load into a mission if you're in a menu)");
		return;
	}

	static const char* kindNames[] = { "WEAPONS", "VEHICLES", "CHARACTERS", "EQUIPMENT", "PROPS" };
	int counts[(int)SpawnKind::Count] = {};
	for (auto& e : *catalogue) counts[(int)e.kind]++;
	if (counts[mSpawnKind] == 0)
		for (int k = 0; k < (int)SpawnKind::Count; k++) if (counts[k]) { mSpawnKind = k; break; }

	// kind pills
	bool first = true;
	for (int k = 0; k < (int)SpawnKind::Count; k++)
	{
		if (counts[k] == 0) continue;
		if (!first) ImGui::SameLine(0.f, 8.f * S);
		first = false;
		auto label = std::format("{}  {}##spawnkind{}", kindNames[k], counts[k], k);
		if (luxButton(label.c_str(), k == mSpawnKind ? Btn::Primary : Btn::Outline, B, S)) { mSpawnKind = k; }
	}
	ImGui::Dummy(ImVec2(0.f, 6.f * S));

	// filter
	{
		float px = B * 0.82f, h = px + 18.f * S;
		ModMenuFonts::Scoped f(ModMenuFonts::body(), px);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.5f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18.f * S, 9.f * S));
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::InputTextWithHint("##spawnfilter", "Filter (rifle, warthog, hunter...)", &mSpawnFilter);
		ImGui::PopStyleVar(2);
	}
	if (mSpawnKind == (int)SpawnKind::Character)
		textItem(ModMenuFonts::body(), B * 0.74f, colMuted, "Characters spawn as empty bodies. Press BECOME to jump straight into one.", ImGui::GetContentRegionAvail().x);
	else if (mSpawnKind == (int)SpawnKind::Vehicle)
		textItem(ModMenuFonts::body(), B * 0.74f, colMuted, "Vehicles appear a few metres ahead of you, facing the way you're looking.", ImGui::GetContentRegionAvail().x);
	ImGui::Dummy(ImVec2(0.f, 4.f * S));

	// rows
	std::string filter = lower(mSpawnFilter);
	std::vector<const ModMenuSpawnEntry*> rows;
	for (auto& e : *catalogue)
		if ((int)e.kind == mSpawnKind && (filter.empty() || lower(e.name).contains(filter) || lower(e.path).contains(filter))) rows.push_back(&e);
	if (rows.empty())
	{
		textItem(ModMenuFonts::body(), B * 0.86f, colTaupe, "Nothing matches that filter.");
		return;
	}

	const float rowH = 54.f * S;
	ImGuiListClipper clipper;
	clipper.Begin((int)rows.size(), rowH + ImGui::GetStyle().ItemSpacing.y);
	while (clipper.Step())
	{
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
		{
			const auto& e = *rows[i];
			ImGui::PushID((int)e.tag);
			ImVec2 p = ImGui::GetCursorScreenPos();
			float w = ImGui::GetContentRegionAvail().x;
			ImVec2 q(p.x + w, p.y + rowH - 6.f * S);
			bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, q);
			dl->AddRectFilled(p, q, col32(hovered ? colCardHover : colCard), 10.f * S);

			float namePx = B * 0.92f, pathPx = B * 0.62f;
			float textTop = p.y + (rowH - 6.f * S - namePx - pathPx - 3.f * S) * 0.5f;
			dl->AddText(fBold(), namePx, ImVec2(p.x + 18.f * S, textTop), col32(colIvory), e.name.c_str());
			ImGui::PushClipRect(p, ImVec2(q.x - 260.f * S, q.y), true);
			dl->AddText(fBody(), pathPx, ImVec2(p.x + 18.f * S, textTop + namePx + 3.f * S), col32(colMuted), e.path.c_str());
			ImGui::PopClipRect();

			bool canBecome = e.kind == SpawnKind::Character;
			float bh = buttonHeight(B, S);
			float spawnW = buttonWidth("SPAWN", Btn::Outline, B, S);
			float becomeW = canBecome ? buttonWidth("BECOME", Btn::Primary, B, S) + 10.f * S : 0.f;
			ImGui::SetCursorScreenPos(ImVec2(q.x - 14.f * S - spawnW - becomeW, p.y + (rowH - 6.f * S - bh) * 0.5f));
			if (canBecome)
			{
				if (luxButton("BECOME", Btn::Primary, B, S) && mSpawnAction) toast(mSpawnAction(e, true));
				ImGui::SameLine(0.f, 10.f * S);
			}
			if (luxButton("SPAWN", Btn::Outline, B, S) && mSpawnAction) toast(mSpawnAction(e, false));
			ImGui::SetCursorScreenPos(p);
			ImGui::Dummy(ImVec2(w, rowH));
			ImGui::PopID();
		}
	}
	clipper.End();
}

void ModMenuGUI::renderLog(float totalHeight)
{
	const float B = mBasePx, S = B / 20.f;
	std::deque<LogLine> copy;
	bool scroll = false;
	{
		std::lock_guard<std::mutex> lock(mLogMutex);
		copy = mLog;
		scroll = mScrollLogToBottom;
		mScrollLogToBottom = false;
	}

	ImGui::Dummy(ImVec2(0.f, 4.f * S));
	trackedItem(fBold(), B * 0.54f, colMuted, "ENGINE LOG  -  EVERY COMMAND SENT TO MCC", 2.6f * S);
	float px = B * 0.72f;
	float lineH = px + ImGui::GetStyle().ItemSpacing.y;
	ImGui::PushStyleColor(ImGuiCol_ChildBg, withAlpha(colInk, 0.65f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f * S, 10.f * S));
	ImGui::BeginChild("##modmenulog", ImVec2(0.f, std::max(lineH * 2.f + 20.f * S, totalHeight - B)), ImGuiChildFlags_AlwaysUseWindowPadding);
	{
		ModMenuFonts::Scoped f(ModMenuFonts::body(), px);
		for (auto& line : copy)
		{
			ImGui::TextColored(colFaint, "%s", line.time.c_str());
			ImGui::SameLine(0.f, 12.f * S);
			switch (line.kind)
			{
			case LogLine::Kind::Command: ImGui::TextColored(colGold, "%s", line.text.c_str()); break;
			case LogLine::Kind::Output: ImGui::TextColored(colIvory, "%s", line.text.c_str()); break;
			case LogLine::Kind::Error: ImGui::TextColored(colDanger, "%s", line.text.c_str()); break;
			case LogLine::Kind::Info: ImGui::TextColored(colTaupe, "%s", line.text.c_str()); break;
			}
		}
	}
	if (scroll) ImGui::SetScrollHereY(1.f);
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
}

void ModMenuGUI::renderSettingsPopup()
{
	const float B = mBasePx, S = B / 20.f;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.f * S, 22.f * S));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f * S, 12.f * S));
	ImGui::SetNextWindowPos(mSettingsAnchor, ImGuiCond_Appearing, ImVec2(1.f, 1.f));
	if (ImGui::BeginPopup("##modmenusettings"))
	{
		trackedItem(fDisplay(), B * 1.05f, colGold, "SETTINGS", 2.2f * S);
		ImGui::Dummy(ImVec2(0.f, 2.f * S));
		goldRule(420.f * S, 0.4f);
		ImGui::Dummy(ImVec2(0.f, 2.f * S));

		auto row = [&](const char* id, bool& value, const char* text)
			{
				ImVec2 sw = switchSize(S);
				float px = B * 0.86f;
				float rowTop = ImGui::GetCursorScreenPos().y;
				if (luxSwitch(id, value, S)) value = !value;
				rowNext(rowTop, sw.y, px, 16.f * S);
				textItem(ModMenuFonts::body(), px, colIvory, text);
			};
		row("##set_reapply", mAutoReapply, "Re-apply mods after death and level loads");
		row("##set_pause", mPauseWhileOpen, "Pause the game while this menu is open");
		row("##set_log", mShowLog, "Show the engine log");

		ImGui::Dummy(ImVec2(0.f, 4.f * S));
		trackedItem(fBold(), B * 0.56f, colMuted, std::format("TEXT SIZE  {}%", (int)std::round(mTextScale * 100.f)), 2.4f * S);
		bool released = false;
		float v = mTextScale;
		if (luxSlider("##set_text", &v, 0.75f, 1.5f, 0.05f, 420.f * S, S, &released)) mTextScale = v;
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(2);
}

void ModMenuGUI::renderFooter()
{
	const float B = mBasePx, S = B / 20.f;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	float W = ImGui::GetContentRegionAvail().x;

	ImGui::Dummy(ImVec2(0.f, 6.f * S));
	goldRule(W, 0.35f);
	ImGui::Dummy(ImVec2(0.f, 8.f * S));

	float rowH = buttonHeight(B, S);
	ImVec2 rowStart = ImGui::GetCursorScreenPos();

	// left: the latest confirmation, fading out, otherwise a quiet summary
	std::string toastText;
	bool toastError = false;
	float toastAge = 99.f;
	{
		std::lock_guard<std::mutex> lock(mLogMutex);
		toastText = mToastText;
		toastError = mToastIsError;
		toastAge = std::chrono::duration<float>(std::chrono::steady_clock::now() - mToastAt).count();
	}
	float px = B * 0.82f;
	float textY = rowStart.y + (rowH - px) * 0.5f;
	float linksW = 0.f;
	for (const char* l : { "RE-APPLY", "ALL OFF", "RELOAD", "FOLDER", "SETTINGS" }) linksW += buttonWidth(l, Btn::Ghost, B, S) + 6.f * S;
	dl->PushClipRect(ImVec2(rowStart.x - 8.f * S, rowStart.y), ImVec2(rowStart.x + std::max(60.f * S, W - linksW - 16.f * S), rowStart.y + rowH), true);
	if (!toastText.empty() && toastAge < 4.f)
	{
		float alpha = std::clamp((4.f - toastAge) / 0.8f, 0.f, 1.f);
		ImVec4 c = toastError ? colDanger : colGoldBright;
		diamond(dl, ImVec2(rowStart.x + 4.f * S, textY + px * 0.55f), 3.5f * S, col32(c, alpha));
		dl->AddText(fBody(), px, ImVec2(rowStart.x + 18.f * S, textY), col32(c, alpha), toastText.c_str());
	}
	else if (mLibraryStatusIsError)
	{
		dl->AddText(fBody(), px, ImVec2(rowStart.x, textY), col32(colDanger), mLibraryStatus.c_str());
	}
	else
	{
		int n = activeCount();
		auto s = n == 0 ? std::string("Nothing switched on") : std::format("{} mod{} switched on", n, n == 1 ? "" : "s");
		dl->AddText(fBody(), px, ImVec2(rowStart.x, textY), col32(colMuted), s.c_str());
	}
	dl->PopClipRect();

	// right: quiet text actions
	struct Link { const char* label; const char* tip; };
	static const Link links[] = {
		{ "RE-APPLY", "Send every active mod again, if one wore off." },
		{ "ALL OFF", "Switch every mod off." },
		{ "RELOAD", "Re-read ModMenuLibrary.xml after editing it." },
		{ "FOLDER", "Open the folder with ModMenuLibrary.xml." },
		{ "SETTINGS", nullptr },
	};
	float total = 0.f;
	for (auto& l : links) total += buttonWidth(l.label, Btn::Ghost, B, S) + 6.f * S;
	ImGui::SetCursorScreenPos(ImVec2(std::max(rowStart.x + W * 0.45f, rowStart.x + W - total), rowStart.y));

	bool first = true;
	for (auto& l : links)
	{
		if (!first) ImGui::SameLine(0.f, 6.f * S);
		first = false;
		if (luxButton(l.label, Btn::Ghost, B, S))
		{
			std::string_view label = l.label;
			if (label == "RE-APPLY") { int n = reapplyActive("manual re-apply"); toast(n > 0 ? std::format("Re-applied {} mod{}", n, n == 1 ? "" : "s") : std::string("Nothing to re-apply")); }
			else if (label == "ALL OFF") { turnEverythingOff(); toast("Everything switched off"); }
			else if (label == "RELOAD") { loadLibrary(); toast(mLibraryStatus, mLibraryStatusIsError); }
			else if (label == "FOLDER")
			{
				auto folder = std::filesystem::path(mLibraryPath).parent_path().string();
				ShellExecuteA(nullptr, "open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
			else if (label == "SETTINGS")
			{
				ImGui::OpenPopup("##modmenusettings");
				mSettingsAnchor = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMin().y - 10.f * S);
			}
		}
		if (l.tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", l.tip);
	}
	renderSettingsPopup();
}

void ModMenuGUI::render(SimpleMath::Vector2 screenSize)
{
	// Body text is 20px on a 1440p screen and scales with resolution, times the Text Size setting.
	mBasePx = std::clamp(screenSize.y / 1440.f * 20.f, 13.f, 40.f) * mTextScale;
	const float B = mBasePx, S = B / 20.f;

	ModMenuFonts::Scoped baseFont(ModMenuFonts::body(), B * 0.86f);
	pushStyle();

	ImGui::SetNextWindowPos(ImVec2(screenSize.x * 0.5f, screenSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(std::min(screenSize.x * 0.66f, 1500.f * S), screenSize.y * 0.84f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(620.f * S, screenSize.x), std::min(440.f * S, screenSize.y)), ImVec2(screenSize.x, screenSize.y));

	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	if (ImGui::Begin("Mod Menu##ModMenuGUI", nullptr, flags))
	{
		renderBackdrop();
		renderHeader();

		float footerH = std::max(mLastFooterH, buttonHeight(B, S) + 16.f * S) + ImGui::GetStyle().ItemSpacing.y;
		float logH = mShowLog ? std::min(B * 9.f, ImGui::GetContentRegionAvail().y * 0.3f) : 0.f;
		float bodyH = std::max(140.f * S, ImGui::GetContentRegionAvail().y - footerH - logH);
		float catW = std::clamp(ImGui::GetContentRegionAvail().x * 0.22f, 190.f * S, 270.f * S);

		ImGui::BeginChild("##modmenucats", ImVec2(catW, bodyH));
		renderCategoryList();
		ImGui::EndChild();

		// hairline between the sidebar and the list
		ImGui::SameLine(0.f, 18.f * S);
		{
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImU32 clear = col32(colGold, 0.f), gold = col32(colGold, 0.22f);
			ImGui::GetWindowDrawList()->AddRectFilledMultiColor(ImVec2(p.x, p.y), ImVec2(p.x + 1.f, p.y + bodyH * 0.5f), clear, clear, gold, gold);
			ImGui::GetWindowDrawList()->AddRectFilledMultiColor(ImVec2(p.x, p.y + bodyH * 0.5f), ImVec2(p.x + 1.f, p.y + bodyH), gold, gold, clear, clear);
			ImGui::Dummy(ImVec2(1.f, bodyH));
		}
		ImGui::SameLine(0.f, 18.f * S);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.f * S, 2.f * S));
		ImGui::BeginChild("##modmenumods", ImVec2(0.f, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
		ImGui::PopStyleVar();
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.f * S, 6.f * S));
		renderModList();
		ImGui::PopStyleVar();
		ImGui::EndChild();

		if (mShowLog) renderLog(logH);
		float footerStartY = ImGui::GetCursorPosY();
		renderFooter();
		mLastFooterH = ImGui::GetCursorPosY() - footerStartY;
	}
	ImGui::End();
	popStyle();
}

// ------------------------------------------------------------------ developer automation

namespace
{
	int64_t steadyMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	// commands that end the mission, kill you or empty the level: a sweep can't carry on after them
	constexpr const char* kSweepBreakers[] = { "(game_won", "(game_lost", "(game_save_and_quit", "(object_destroy_all", "(unit_kill", "(ai_erase_all" };

	const char* typeName(ModType t)
	{
		switch (t)
		{
		case ModType::Toggle: return "toggle";
		case ModType::Slider: return "slider";
		case ModType::Button: return "button";
		case ModType::Choice: return "choice";
		case ModType::Text: return "text";
		case ModType::HCMToggle: return "hcm-toggle";
		case ModType::HCMSlider: return "hcm-slider";
		case ModType::HCMAction: return "hcm-action";
		case ModType::Skull: return "skull";
		}
		return "?";
	}

	std::string trimmed(const std::string& s)
	{
		auto a = s.find_first_not_of(" \t\r\n");
		if (a == std::string::npos) return "";
		auto b = s.find_last_not_of(" \t\r\n");
		return s.substr(a, b - a + 1);
	}
}

void ModMenuGUI::devLog(const std::string& text)
{
	if (mDevLog) mDevLog(text);
	addLog(text, LogLine::Kind::Info);
}

const ModDef* ModMenuGUI::findMod(const std::string& id)
{
	for (auto& cat : mLibrary.categories)
		for (auto& mod : cat.mods)
			if (mod.id == id) return &mod;
	return nullptr;
}

bool ModMenuGUI::sweepSkipReason(const ModDef& mod, std::string& why)
{
	switch (mod.type)
	{
	case ModType::Toggle: case ModType::Slider: case ModType::Button: case ModType::Choice: break;
	default: why = std::format("{} mods aren't swept", typeName(mod.type)); return true;
	}
	auto cmds = mod.commandsFor(mGame);
	if (!cmds) { why = "no commands for this game"; return true; }
	for (auto* list : { &cmds->on, &cmds->off, &cmds->set, &cmds->apply, &cmds->run })
		for (auto& line : *list)
			for (auto* breaker : kSweepBreakers)
				if (line.contains(breaker)) { why = std::format("uses {})", breaker); return true; }
	return false;
}

std::string ModMenuGUI::startSelfTest(const std::string& filter)
{
	SelfTest test;
	for (auto& cat : mLibrary.categories)
	{
		if (!cat.special.empty()) continue;
		for (auto& mod : cat.mods)
		{
			if (!mod.supports(mGame)) continue;
			if (!filter.empty() && filter != "all" && cat.id != filter && !mod.id.starts_with(filter)) continue;
			std::string why;
			if (sweepSkipReason(mod, why))
			{
				devLog(std::format("SKIP {} ({})", mod.id, why));
				test.skipped++;
				continue;
			}
			test.mods.push_back(&mod);
		}
	}
	if (test.mods.empty()) return std::format("self-test: nothing matches '{}'", filter);
	turnEverythingOff();
	test.nextAtMs = steadyMs() + 500;
	auto count = test.mods.size();
	auto skipped = test.skipped;
	mSelfTest = std::move(test);
	auto text = std::format("SELFTEST START {} mods ({} skipped) in {}", count, skipped, mGame.toString());
	devLog(text);
	toast(std::format("Self-test: running {} mods...", count));
	return text;
}

void ModMenuGUI::tickSelfTest(bool gamePlaying)
{
	if (!mSelfTest) return;
	auto now = steadyMs();
	if (now < mSelfTest->nextAtMs) return;
	if (!gamePlaying) { mSelfTest->nextAtMs = now + 500; return; } // loading screen, pause menu: wait

	auto& t = *mSelfTest;
	if (t.index >= t.mods.size())
	{
		turnEverythingOff();
		devLog(std::format("SELFTEST DONE: {} mods ran and the game is still up", t.mods.size()));
		toast("Self-test finished. Everything was turned back off.");
		mSelfTest.reset();
		return;
	}

	const ModDef& mod = *t.mods[t.index];
	if (t.phase == 0) devLog(std::format("TEST {}/{} {} [{}] {}", t.index + 1, t.mods.size(), mod.id, typeName(mod.type), mod.name));

	int wait = 1200;
	bool done = false;
	switch (mod.type)
	{
	case ModType::Toggle:
		if (t.phase == 0) applyToggle(mod, true);
		else if (t.phase == 1) { applyToggle(mod, false); wait = 500; }
		else done = true;
		break;
	case ModType::Slider:
		if (t.phase == 0) applySlider(mod, mod.maxValue);
		else if (t.phase == 1) applySlider(mod, mod.minValue);
		else if (t.phase == 2)
		{
			bool hasDefault = mod.defaultValue >= mod.minValue && mod.defaultValue <= mod.maxValue;
			applySlider(mod, hasDefault ? mod.defaultValue : mod.minValue);
			runtime(mod).applied = false;
			wait = 500;
		}
		else done = true;
		break;
	case ModType::Choice:
	{
		int count = (int)mod.options.size();
		if (t.phase < count)
		{
			if (mod.options[t.phase].supports(mGame)) applyChoice(mod, t.phase);
			else wait = 0;
		}
		else if (t.phase == count) { undoChoice(mod); wait = 500; }
		else done = true;
		break;
	}
	case ModType::Button:
		if (t.phase == 0) { runButton(mod); wait = 1500; }
		else done = true;
		break;
	default:
		done = true;
		break;
	}

	if (done)
	{
		devLog(std::format("  ok {}", mod.id));
		// a form (or its reset) leaves the player changed: put everything back before the next mod
		if (mod.type == ModType::Button && !mod.group.empty()) turnEverythingOff();
		t.index++;
		t.phase = 0;
		t.nextAtMs = now + 250;
		return;
	}
	t.phase++;
	t.nextAtMs = now + wait;
}

std::string ModMenuGUI::devCommand(const std::string& rawLine)
{
	std::string line = trimmed(rawLine);
	auto space = line.find(' ');
	std::string verb = lower(line.substr(0, space));
	std::string rest = space == std::string::npos ? "" : trimmed(line.substr(space + 1));

	if (verb == "selftest")
	{
		if (rest == "stop")
		{
			if (!mSelfTest) return "no self-test running";
			mSelfTest.reset();
			turnEverythingOff();
			return "self-test stopped, everything off";
		}
		if (mSelfTest) return "a self-test is already running ('selftest stop' first)";
		return startSelfTest(rest);
	}
	if (verb == "alloff") { turnEverythingOff(); return "turned everything off"; }
	if (verb == "hs")
	{
		if (rest.empty()) return "usage: hs (some_command args)";
		sendLines({ rest }, "");
		return "sent";
	}
	if (verb == "mods")
	{
		int n = 0;
		for (auto& cat : mLibrary.categories)
			for (auto& mod : cat.mods)
				if (mod.supports(mGame))
				{
					devLog(std::format("  {} / {} [{}] {}", cat.id, mod.id, typeName(mod.type), mod.name));
					n++;
				}
		return std::format("{} mods in {}", n, mGame.toString());
	}
	if (verb == "mod")
	{
		auto space2 = rest.find(' ');
		std::string id = rest.substr(0, space2);
		std::string arg = space2 == std::string::npos ? "" : trimmed(rest.substr(space2 + 1));
		auto* mod = findMod(id);
		if (!mod) return std::format("no mod '{}'", id);
		if (!mod->supports(mGame)) return std::format("{} isn't in {}", id, mGame.toString());
		try
		{
			switch (mod->type)
			{
			case ModType::Toggle: applyToggle(*mod, arg != "off"); return std::format("{} {}", id, arg != "off" ? "on" : "off");
			case ModType::Slider: applySlider(*mod, arg.empty() ? mod->maxValue : std::stof(arg)); return std::format("{} = {}", id, runtime(*mod).value);
			case ModType::Choice:
			{
				if (arg == "off") { undoChoice(*mod); return std::format("{} off", id); }
				int i = arg.empty() ? 0 : std::stoi(arg);
				if (i < 0 || i >= (int)mod->options.size()) return std::format("{} has {} options", id, mod->options.size());
				applyChoice(*mod, i);
				return std::format("{} = {}", id, mod->options[i].label);
			}
			case ModType::Button: runButton(*mod); return std::format("ran {}", id);
			case ModType::Text: runText(*mod, arg); return std::format("{} '{}'", id, arg);
			case ModType::HCMToggle: setHcmBool(*mod, arg != "off"); return std::format("{} {}", id, arg != "off" ? "on" : "off");
			case ModType::HCMAction:
				if (mod->hcmKey.starts_with("possess")) return mCustomAction ? mCustomAction(mod->hcmKey) : "possession unavailable";
				if (auto ev = hcmAction(mod->hcmKey)) { ev->operator()(); return std::format("fired {}", mod->hcmKey); }
				return std::format("unknown HCM action {}", mod->hcmKey);
			default: return std::format("{} mods can't be run from here", typeName(mod->type));
			}
		}
		catch (const std::exception& ex)
		{
			return std::format("bad value '{}': {}", arg, ex.what());
		}
	}
	return std::format("unknown command '{}' (try: mods, mod <id> [on|off|value], selftest [category|all|stop], alloff, hs <command>, spawn <name>, become <name>, catalogue, possess, return, pose, wait <ms>)", verb);
}
