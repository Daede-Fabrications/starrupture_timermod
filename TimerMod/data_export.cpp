#include "data_export.h"
#include "plugin_config.h"
#include "plugin_helpers.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdarg>
#include <fstream>

namespace DataExport
{

static float s_accumulator = 0.0f;

// ---------------------------------------------------------------------------
// Build a simple JSON string from the timer state.
// No external JSON library needed — the structure is fixed and simple.
// Negative float values are written as JSON null (unknown / not applicable).
// ---------------------------------------------------------------------------

// Append a formatted string to buf at offset *pos, updating *pos on success.
static void Append(char* buf, int bufSize, int* pos, const char* fmt, ...)
{
	if (*pos < 0 || *pos >= bufSize) return;
	va_list args;
	va_start(args, fmt);
	int written = _vsnprintf_s(buf + *pos, bufSize - *pos, _TRUNCATE, fmt, args);
	va_end(args);
	if (written > 0) *pos += written;
}

// Write a JSON key: value line where negative value → null.
static void AppendNullableFloat(char* buf, int bufSize, int* pos, const char* key, float val, bool comma)
{
	if (val < 0.0f)
		Append(buf, bufSize, pos, "  \"%s\": null%s\n", key, comma ? "," : "");
	else
		Append(buf, bufSize, pos, "  \"%s\": %.1f%s\n", key, val, comma ? "," : "");
}

static void WriteJson(const char* filePath, const RuptureTimer::TimerState& state)
{
	char buf[1024];
	int pos = 0;

	if (!state.valid)
	{
		Append(buf, sizeof(buf), &pos, "{\n  \"valid\": false\n}\n");
	}
	else
	{
		const bool extended = RuptureTimerConfig::Config::ShouldWriteExtendedPhaseTimers();

		Append(buf, sizeof(buf), &pos, "{\n");
		Append(buf, sizeof(buf), &pos, "  \"valid\": true,\n");
		Append(buf, sizeof(buf), &pos, "  \"phase\": \"%s\",\n",    state.phaseName);
		AppendNullableFloat(buf, sizeof(buf), &pos, "phase_remaining_sec", state.phaseRemainingSeconds, true);
		AppendNullableFloat(buf, sizeof(buf), &pos, "next_rupture_in_sec", state.nextRuptureInSeconds,  true);
		Append(buf, sizeof(buf), &pos, "  \"wave_number\": %d,\n",  state.waveNumber);
		Append(buf, sizeof(buf), &pos, "  \"wave_type\": \"%s\",\n", state.waveTypeName);

		if (extended)
		{
			Append(buf, sizeof(buf), &pos, "  \"paused\": %s,\n", state.paused ? "true" : "false");
			AppendNullableFloat(buf, sizeof(buf), &pos, "warning_remaining_sec",     state.warningRemaining,     true);
			AppendNullableFloat(buf, sizeof(buf), &pos, "burning_remaining_sec",     state.burningRemaining,     true);
			AppendNullableFloat(buf, sizeof(buf), &pos, "cooling_remaining_sec",     state.coolingRemaining,     true);
			AppendNullableFloat(buf, sizeof(buf), &pos, "stabilizing_remaining_sec", state.stabilizingRemaining, true);
			AppendNullableFloat(buf, sizeof(buf), &pos, "stable_remaining_sec",      state.stableRemaining,      false);
		}
		else
		{
			Append(buf, sizeof(buf), &pos, "  \"paused\": %s\n", state.paused ? "true" : "false");
		}

		Append(buf, sizeof(buf), &pos, "}\n");
	}

	std::ofstream f(filePath, std::ios::out | std::ios::trunc);
	if (!f.is_open())
	{
		LOG_WARN("DataExport: failed to open %s for writing", filePath);
		return;
	}

	f << buf;
	f.close();
}

void EnsureOutputDir()
{
	const char* path = RuptureTimerConfig::Config::GetJsonFilePath();

	// Find last separator to extract directory part
	const char* lastSlash = nullptr;
	for (const char* p = path; *p; p++)
		if (*p == '/' || *p == '\\') lastSlash = p;

	if (!lastSlash) return;

	char dirBuf[512];
	size_t dirLen = static_cast<size_t>(lastSlash - path);
	if (dirLen >= sizeof(dirBuf)) return;

	memcpy(dirBuf, path, dirLen);
	dirBuf[dirLen] = '\0';

	CreateDirectoryA(dirBuf, nullptr);
	// Ignore ERROR_ALREADY_EXISTS — that's fine
}

void Update(float deltaSeconds, const RuptureTimer::TimerState& state)
{
	if (!RuptureTimerConfig::Config::ShouldWriteJsonFile())
	{
		static bool s_warned = false;
		if (!s_warned) { s_warned = true; LOG_WARN("DataExport: WriteJsonFile is disabled in config — JSON will not update"); }
		return;
	}

	s_accumulator += deltaSeconds;
	float interval = RuptureTimerConfig::Config::GetUpdateIntervalSeconds();
	if (interval < 0.1f) interval = 0.1f;

	if (s_accumulator < interval) return;
	s_accumulator = 0.0f;

	LOG_DEBUG("DataExport: writing %s (phase=%s remaining=%.1f)",
		RuptureTimerConfig::Config::GetJsonFilePath(),
		state.phaseName ? state.phaseName : "?",
		state.phaseRemainingSeconds);
	WriteJson(RuptureTimerConfig::Config::GetJsonFilePath(), state);
}

// ---------------------------------------------------------------------------
// Diagnostic log — appends one line per state change or periodic heartbeat.
// Format: CSV with header row, suitable for analysis in a spreadsheet.
// ---------------------------------------------------------------------------

static float s_diagAccumulator      = 0.0f;
static float s_diagHeartbeatAccum   = 0.0f;
static constexpr float DIAG_MIN_INTERVAL = 1.0f;   // don't log more often than 1s
static constexpr float DIAG_HEARTBEAT    = 30.0f;  // force a log line every 30s even if nothing changed
static bool  s_diagHeaderWritten    = false;

// Previous values for change detection
static int      s_prevRawStage     = -99;
static int      s_prevRawWaveType  = -99;
static int32_t  s_prevRawNextPhase = -99;
static bool     s_prevRawPaused    = false;
static float    s_prevRemaining    = -99.0f;
static const char* s_prevCodePath  = "";

// Separate tracking for immediate phase-change log lines (bypasses the 1s throttle).
static int32_t s_immediatePrevNextPhase = -99;
static int     s_immediatePrevRawStage  = -99;

static bool DiagStateChanged(const RuptureTimer::TimerState& state)
{
	const auto& d = state.diag;
	// Phase/stage/path changes are always significant
	if (d.rawStage != s_prevRawStage) return true;
	if (d.rawWaveType != s_prevRawWaveType) return true;
	if (d.rawNextPhase != s_prevRawNextPhase) return true;
	if (d.rawPaused != s_prevRawPaused) return true;
	if (d.codePath != s_prevCodePath) return true;
	// Timer jumps > 5s are significant (indicates phase transition or correction)
	float remainDelta = d.rawNextTimeRemaining - s_prevRemaining;
	if (remainDelta > 5.0f || remainDelta < -5.0f) return true;
	return false;
}

static void UpdatePrevDiagState(const RuptureTimer::TimerState& state)
{
	const auto& d = state.diag;
	s_prevRawStage     = d.rawStage;
	s_prevRawWaveType  = d.rawWaveType;
	s_prevRawNextPhase = d.rawNextPhase;
	s_prevRawPaused    = d.rawPaused;
	s_prevRemaining    = d.rawNextTimeRemaining;
	s_prevCodePath     = d.codePath;
}

static void WriteDiagLine(const char* filePath, const RuptureTimer::TimerState& state, const char* reason)
{
	std::ofstream f(filePath, std::ios::out | std::ios::app);
	if (!f.is_open()) return;

	const auto& d = state.diag;

	// Format hex dump of repActor bytes
	char hexBuf[32] = "N/A";
	if (d.repActorBytesValid)
	{
		_snprintf_s(hexBuf, sizeof(hexBuf), _TRUNCATE,
			"%02X %02X %02X %02X %02X %02X %02X %02X",
			d.repActorBytes[0], d.repActorBytes[1], d.repActorBytes[2], d.repActorBytes[3],
			d.repActorBytes[4], d.repActorBytes[5], d.repActorBytes[6], d.repActorBytes[7]);
	}

	char line[512];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
		"%.1f,%s,%s,%d,%d,%.1f,%.1f,%.4f,%d,%s,%s,%.4f,%s,%s,%.1f,%.1f,%s\n",
		d.rawServerTime,                              // serverTime
		reason,                                       // trigger (change/heartbeat/phaseChange)
		d.codePath,                                   // path
		d.rawStage,                                   // rawStage
		d.rawWaveType,                                // rawWave
		d.rawNextTime,                                // nextTime (absolute)
		d.rawNextTimeRemaining,                       // remaining
		d.rawProgress,                                // progress (subsystem only)
		d.rawNextPhase,                               // nextPhase (raw int)
		d.rawPhaseName ? d.rawPhaseName : "?",        // rawPhaseName (game-internal label)
		d.rawPaused ? "Y" : "N",                     // paused
		state.phaseRemainingSeconds,                  // computed phaseRem
		state.phaseName ? state.phaseName : "?",      // computed phase
		state.waveTypeName ? state.waveTypeName : "?", // computed waveType
		state.nextRuptureInSeconds,                   // computed nextRup
		state.stableRemaining,                        // computed stableRem
		hexBuf);                                      // repActor hex dump

	f << line;
	f.close();
}

void EnsureDiagnosticLogDir()
{
	if (!RuptureTimerConfig::Config::ShouldWriteDiagnosticLog())
		return;

	const char* path = RuptureTimerConfig::Config::GetDiagnosticLogPath();

	// Extract directory part
	const char* lastSlash = nullptr;
	for (const char* p = path; *p; p++)
		if (*p == '/' || *p == '\\') lastSlash = p;

	if (lastSlash)
	{
		char dirBuf[512];
		size_t dirLen = static_cast<size_t>(lastSlash - path);
		if (dirLen < sizeof(dirBuf))
		{
			memcpy(dirBuf, path, dirLen);
			dirBuf[dirLen] = '\0';
			CreateDirectoryA(dirBuf, nullptr);
		}
	}

	// Write CSV header if file doesn't exist or is empty
	if (!s_diagHeaderWritten)
	{
		// Check if file has content
		std::ifstream check(path, std::ios::ate);
		bool needHeader = !check.is_open() || check.tellg() == 0;
		check.close();

		if (needHeader)
		{
			std::ofstream f(path, std::ios::out | std::ios::app);
			if (f.is_open())
			{
				f << "serverTime,trigger,path,rawStage,rawWave,nextTime,remaining,progress,nextPhase,rawPhaseName,paused,phaseRem,phase,waveType,nextRup,stableRem,repActorHex\n";
				f.close();
			}
		}
		s_diagHeaderWritten = true;
	}
}

void UpdateDiagnosticLog(float deltaSeconds, const RuptureTimer::TimerState& state)
{
	if (!RuptureTimerConfig::Config::ShouldWriteDiagnosticLog())
		return;

	if (!state.valid)
		return;

	// Lazy init — handles the case where logging is enabled via the in-game
	// config UI after world begin play (EnsureDiagnosticLogDir is called there
	// at startup, but may have been skipped if the option was off at that time).
	if (!s_diagHeaderWritten)
		EnsureDiagnosticLogDir();

	s_diagAccumulator    += deltaSeconds;
	s_diagHeartbeatAccum += deltaSeconds;

	// Immediate phase-change line — bypasses the 1s throttle so that short
	// phases (e.g. a 13 s Warning) are always captured at the exact tick the
	// game changes NextPhase or EEnviroWaveStage.
	const auto& d = state.diag;
	bool immediatePhaseChange = (d.rawNextPhase != s_immediatePrevNextPhase ||
	                             d.rawStage     != s_immediatePrevRawStage);
	if (immediatePhaseChange)
	{
		s_immediatePrevNextPhase = d.rawNextPhase;
		s_immediatePrevRawStage  = d.rawStage;
		WriteDiagLine(RuptureTimerConfig::Config::GetDiagnosticLogPath(), state, "phaseChange");
		UpdatePrevDiagState(state);
		s_diagAccumulator = 0.0f; // reset throttle — next change line in >=1s
	}

	// Throttle: don't log more than once per second for non-phase-change events
	if (s_diagAccumulator < DIAG_MIN_INTERVAL)
		return;
	s_diagAccumulator = 0.0f;

	bool changed   = DiagStateChanged(state);
	bool heartbeat = (s_diagHeartbeatAccum >= DIAG_HEARTBEAT);

	if (!changed && !heartbeat)
		return;

	const char* reason = changed ? "change" : "heartbeat";
	if (heartbeat) s_diagHeartbeatAccum = 0.0f;

	WriteDiagLine(RuptureTimerConfig::Config::GetDiagnosticLogPath(), state, reason);
	UpdatePrevDiagState(state);
}

} // namespace DataExport
