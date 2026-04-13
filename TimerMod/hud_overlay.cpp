#include "hud_overlay.h"
#include "plugin_config.h"
#include "plugin_helpers.h"

// Win32 headers define DrawText as a macro (DrawTextW/DrawTextA depending on
// the character-set setting). Undefine it BEFORE including Engine_classes.hpp
// so the preprocessor does not rename AHUD::DrawText to AHUD::DrawTextW inside
// the class definition itself.
#ifdef DrawText
#undef DrawText
#endif

#include "Engine_classes.hpp"

#include <cstdio>
#include <cstring>

namespace HudOverlay
{

// ---------------------------------------------------------------------------
// Shared timer state — written by SetState() on the game tick,
// read by Hooked_PostRender() on the render tick.
// Both callbacks execute on the same game thread, so no locking is needed.
// ---------------------------------------------------------------------------
static RuptureTimer::TimerState s_state = {};

// ---------------------------------------------------------------------------
// Smooth display values — interpolated at wall-clock rate in OnPostRender.
//
// Problem: GetServerWorldTimeSeconds() is periodically corrected by the server.
// Each correction causes nextRuptureInSeconds (= NextTime - serverTime) to
// jump by the correction delta, which looks like a visible snap on the HUD.
//
// Fix: maintain locally-interpolated display values that count down at real
// wall-clock rate between server updates.  Only snap to the server value when
// the discrepancy is larger than SNAP_THRESHOLD (catches genuine phase
// transitions like Stable→Warning which change remaining by hundreds of
// seconds) or when the phase itself changes.
//
// SNAP_THRESHOLD: server-clock corrections are typically < 2 s in UE5 net
// play.  3 s absorbs those while still catching legitimate phase-change jumps.
// ---------------------------------------------------------------------------
static constexpr float SNAP_THRESHOLD = 3.0f;

static float s_dispNextRup   = -1.0f;
static float s_dispPhaseRem  = -1.0f;
static float s_dispStableRem = -1.0f;

static RuptureTimer::RupturePhase s_prevPhase = RuptureTimer::RupturePhase::Unknown;

static LARGE_INTEGER s_qpcFreq      = {};
static LARGE_INTEGER s_lastQpcTime  = {};
static bool          s_qpcReady     = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Format seconds as "M:SS" (e.g. 2550 → "42:30"). Returns "--:--" for
// values < 0 (unknown) and "NOW" when seconds == 0 and isNow is true.
static void FormatTime(char* buf, int bufSize, float seconds, bool nowOnZero = false)
{
	if (seconds < 0.0f)
	{
		_snprintf_s(buf, bufSize, _TRUNCATE, "--:--");
		return;
	}
	if (nowOnZero && seconds < 0.5f)
	{
		_snprintf_s(buf, bufSize, _TRUNCATE, "NOW");
		return;
	}
	int total = static_cast<int>(seconds);
	int m     = total / 60;
	int s     = total % 60;
	_snprintf_s(buf, bufSize, _TRUNCATE, "%d:%02d", m, s);
}

// Draw a single line of text with a drop shadow for readability over any
// background. Uses AHUD::DrawText with default engine font (Font = nullptr).
static void DrawLine(SDK::AHUD* hud, float x, float y, float scale, const char* text)
{
	// Convert ASCII to wide string — our text is always plain ASCII digits/letters.
	wchar_t wbuf[128] = {};
	for (int i = 0; text[i] && i < 126; ++i)
		wbuf[i] = static_cast<wchar_t>(text[i]);

	SDK::FString fs(wbuf);
	SDK::FLinearColor shadow{0.0f, 0.0f, 0.0f, 0.75f};
	SDK::FLinearColor white{1.0f, 1.0f, 1.0f, 1.0f};

	// Shadow pass (offset 1 pixel)
	hud->DrawText(fs, shadow, x + 1.0f, y + 1.0f, nullptr, scale, false);
	// Main pass
	hud->DrawText(fs, white,  x,         y,         nullptr, scale, false);
}

// ---------------------------------------------------------------------------
// Position calculation
//
// Anchor names map to 7 points around the screen edge. Text is always
// rendered left-to-right from the calculated origin x/y.
//
// estimatedBlockW — approximate pixel width of the widest text line at
//                   scale 1.0 (default font ~8px/char, longest line ~24 chars).
// lineH           — approximate line height including spacing.
// ---------------------------------------------------------------------------
static constexpr float MARGIN             = 20.0f;
static constexpr float ESTIMATED_CHAR_W  = 12.0f; // pixels per char at scale 1.0 (UE default HUD font)
static constexpr int   LONGEST_LINE_CHARS = 28;   // "Planet: Stabilizing (Heat)" = 26 + buffer
static constexpr float LINE_H_BASE        = 20.0f; // px per line at scale 1.0

static void CalcPosition(const char* posName, float scale,
                         float screenW, float screenH,
                         int lineCount,
                         float& outX, float& outY)
{
	const float margin    = MARGIN * scale;
	const float blockW    = ESTIMATED_CHAR_W * LONGEST_LINE_CHARS * scale;
	const float blockH    = LINE_H_BASE * static_cast<float>(lineCount) * scale;

	// Default: lower-left
	outX = margin;
	outY = screenH - margin - blockH;

	if (_stricmp(posName, "LowerLeft") == 0)
	{
		outX = margin;
		outY = screenH - margin - blockH;
	}
	else if (_stricmp(posName, "MidLeft") == 0)
	{
		outX = margin;
		outY = screenH * 0.5f - blockH * 0.5f;
	}
	else if (_stricmp(posName, "TopLeft") == 0)
	{
		outX = margin;
		outY = margin;
	}
	else if (_stricmp(posName, "TopMid") == 0)
	{
		outX = screenW * 0.5f - blockW * 0.5f;
		outY = margin;
	}
	else if (_stricmp(posName, "TopRight") == 0)
	{
		outX = screenW - margin - blockW;
		outY = margin;
	}
	else if (_stricmp(posName, "MidRight") == 0)
	{
		outX = screenW - margin - blockW;
		outY = screenH * 0.5f - blockH * 0.5f;
	}
	else if (_stricmp(posName, "LowerRight") == 0)
	{
		outX = screenW - margin - blockW;
		outY = screenH - margin - blockH;
	}
}

// ---------------------------------------------------------------------------
// PostRender callback — registered via hooks->HUD->RegisterOnPostRender (v16)
// ---------------------------------------------------------------------------

static void OnPostRender(void* hudPtr)
{
	auto* self = static_cast<SDK::AHUD*>(hudPtr);

	if (!self || !self->Canvas)
		return;

	if (!RuptureTimerConfig::Config::ShouldShowOverlay())
		return;

	if (!s_state.valid)
		return;

	// -----------------------------------------------------------------------
	// Step 1 — compute wall-clock frame delta via QPC.
	// -----------------------------------------------------------------------
	LARGE_INTEGER qpcNow;
	QueryPerformanceCounter(&qpcNow);

	float dt = 0.0f;
	if (!s_qpcReady)
	{
		QueryPerformanceFrequency(&s_qpcFreq);
		s_qpcReady    = true;
		s_lastQpcTime = qpcNow;
	}
	else if (s_qpcFreq.QuadPart > 0)
	{
		dt = static_cast<float>(qpcNow.QuadPart - s_lastQpcTime.QuadPart)
		     / static_cast<float>(s_qpcFreq.QuadPart);
		// Guard against stalls / debugger pauses warping the display.
		if (dt > 2.0f) dt = 0.0f;
	}
	s_lastQpcTime = qpcNow;

	// -----------------------------------------------------------------------
	// Step 2 — tick display values down at wall-clock rate.
	// -----------------------------------------------------------------------
	if (s_dispNextRup   >= 0.0f) s_dispNextRup   -= dt;
	if (s_dispPhaseRem  >= 0.0f) s_dispPhaseRem  -= dt;
	if (s_dispStableRem >= 0.0f) s_dispStableRem -= dt;

	// -----------------------------------------------------------------------
	// Step 3 — sync from server when needed.
	//   • Phase changed            → always snap (new cycle, values are stale).
	//   • Display uninitialised    → snap.
	//   • Discrepancy > threshold  → snap (genuine phase-transition jump).
	//   • Small discrepancy        → ignore; local interpolation is smoother.
	// -----------------------------------------------------------------------
	bool phaseChanged = (s_state.phase != s_prevPhase);
	s_prevPhase = s_state.phase;

	auto Sync = [phaseChanged](float& disp, float srv)
	{
		if (srv < 0.0f)          { disp = srv; return; } // unknown → pass through
		if (disp < 0.0f)         { disp = srv; return; } // uninitialised
		if (phaseChanged)        { disp = srv; return; } // phase flip
		if (disp - srv > SNAP_THRESHOLD ||
		    srv - disp > SNAP_THRESHOLD) { disp = srv; } // large jump
		// else: keep locally interpolated value
	};

	Sync(s_dispNextRup,   s_state.nextRuptureInSeconds);
	Sync(s_dispPhaseRem,  s_state.phaseRemainingSeconds);
	Sync(s_dispStableRem, s_state.stableRemaining);

	if (s_dispNextRup   < 0.0f) s_dispNextRup   = 0.0f;
	if (s_dispPhaseRem  < 0.0f) s_dispPhaseRem  = 0.0f;
	if (s_dispStableRem < 0.0f) s_dispStableRem = 0.0f;

	SDK::UCanvas* canvas = self->Canvas;
	const float screenW = static_cast<float>(canvas->SizeX);
	const float screenH = static_cast<float>(canvas->SizeY);
	if (screenW <= 0.0f || screenH <= 0.0f)
		return;

	const float scale = RuptureTimerConfig::Config::GetOverlayScale();
	const float lineH = LINE_H_BASE * scale;

	const bool extended  = RuptureTimerConfig::Config::ShouldWriteExtendedPhaseTimers();
	const bool debugInfo = RuptureTimerConfig::Config::ShouldShowDebugInfo();

	// Count active extended lines (only for non-Stable phases)
	int extendedLines = 0;
	if (extended && s_state.phase != RuptureTimer::RupturePhase::Stable)
	{
		if (s_state.warningRemaining     >= 0.0f) extendedLines++;
		if (s_state.burningRemaining     >= 0.0f) extendedLines++;
		if (s_state.coolingRemaining     >= 0.0f) extendedLines++;
		if (s_state.stabilizingRemaining >= 0.0f) extendedLines++;
		if (s_state.stableRemaining      >= 0.0f) extendedLines++;
	}

	const int debugLines = debugInfo ? 2 : 0;
	const int totalLines = 3 + extendedLines + debugLines;

	float x, y;
	CalcPosition(RuptureTimerConfig::Config::GetOverlayPosition(), scale, screenW, screenH, totalLines, x, y);

	float curY = y;

	// --- Line 1: Next Rupture countdown ---
	char nextBuf[16];
	FormatTime(nextBuf, sizeof(nextBuf), s_state.nextRuptureInSeconds >= 0.0f ? s_dispNextRup : -1.0f, /*nowOnZero=*/true);

	char line1[48];
	_snprintf_s(line1, sizeof(line1), _TRUNCATE, "Next Rupture: %s", nextBuf);
	DrawLine(self, x, curY, scale, line1);
	curY += lineH;

	// --- Line 2: Planet status (phase + wave type if active) ---
	char line2[48];
	const bool waveActive = (s_state.waveType != 0); // 0 = None
	if (waveActive)
		_snprintf_s(line2, sizeof(line2), _TRUNCATE, "Planet: %s (%s)", s_state.phaseName, s_state.waveTypeName);
	else
		_snprintf_s(line2, sizeof(line2), _TRUNCATE, "Planet: %s", s_state.phaseName);
	DrawLine(self, x, curY, scale, line2);
	curY += lineH;

	// --- Line 3: Current phase timer ---
	char phaseBuf[16];
	FormatTime(phaseBuf, sizeof(phaseBuf), s_state.phaseRemainingSeconds >= 0.0f ? s_dispPhaseRem : -1.0f);

	char line3[48];
	_snprintf_s(line3, sizeof(line3), _TRUNCATE, "Wave Timer: %s", phaseBuf);
	DrawLine(self, x, curY, scale, line3);
	curY += lineH;

	// --- Extended phase breakdown (ExtendedPhaseTimers=true, non-Stable phases only) ---
	if (extended && s_state.phase != RuptureTimer::RupturePhase::Stable)
	{
		char buf[48];
		char tbuf[16];

		if (s_state.warningRemaining >= 0.0f)
		{
			FormatTime(tbuf, sizeof(tbuf), s_state.warningRemaining);
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "  Warning:     %s", tbuf);
			DrawLine(self, x, curY, scale, buf);
			curY += lineH;
		}
		if (s_state.burningRemaining >= 0.0f)
		{
			FormatTime(tbuf, sizeof(tbuf), s_state.burningRemaining);
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "  Burning:     %s", tbuf);
			DrawLine(self, x, curY, scale, buf);
			curY += lineH;
		}
		if (s_state.coolingRemaining >= 0.0f)
		{
			FormatTime(tbuf, sizeof(tbuf), s_state.coolingRemaining);
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "  Cooling:     %s", tbuf);
			DrawLine(self, x, curY, scale, buf);
			curY += lineH;
		}
		if (s_state.stabilizingRemaining >= 0.0f)
		{
			FormatTime(tbuf, sizeof(tbuf), s_state.stabilizingRemaining);
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "  Stabilizing: %s", tbuf);
			DrawLine(self, x, curY, scale, buf);
			curY += lineH;
		}
		if (s_state.stableRemaining >= 0.0f)
		{
			FormatTime(tbuf, sizeof(tbuf), s_dispStableRem);
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "  Stable:      %s", tbuf);
			DrawLine(self, x, curY, scale, buf);
			curY += lineH;
		}
	}

	// --- Debug info lines (ShowDebugInfo=true) ---
	if (debugInfo)
	{
		char dbg1[80];
		_snprintf_s(dbg1, sizeof(dbg1), _TRUNCATE, "[Wave:%d RawStage:%d Path:%s]",
			s_state.waveNumber,
			s_state.diag.rawStage,
			s_state.diag.codePath ? s_state.diag.codePath : "?");
		DrawLine(self, x, curY, scale, dbg1);
		curY += lineH;

		char dbg2[64];
		_snprintf_s(dbg2, sizeof(dbg2), _TRUNCATE, "[PhRem:%.1f Rup:%.1f %s]",
			s_state.phaseRemainingSeconds,
			s_state.nextRuptureInSeconds,
			s_state.paused ? "PAUSED" : "");
		DrawLine(self, x, curY, scale, dbg2);
	}
}

// ---------------------------------------------------------------------------
// Install / Remove
// ---------------------------------------------------------------------------

bool Install(IPluginHooks* hooks)
{
	if (!hooks || !hooks->HUD)
	{
		LOG_DEBUG("[HudOverlay] hooks->HUD not available — overlay disabled (server build or pre-v16 loader)");
		return false;
	}

	hooks->HUD->RegisterOnPostRender(OnPostRender);
	LOG_INFO("[HudOverlay] PostRender callback registered via v16 HUD interface");
	return true;
}

void Remove(IPluginHooks* hooks)
{
	if (!hooks || !hooks->HUD)
		return;

	hooks->HUD->UnregisterOnPostRender(OnPostRender);
	LOG_INFO("[HudOverlay] PostRender callback unregistered");
}

void SetState(const RuptureTimer::TimerState& state)
{
	s_state = state;
}

} // namespace HudOverlay
