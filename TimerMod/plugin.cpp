#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "timer_tracker.h"
#include "data_export.h"
#include "hud_overlay.h"
#include "plugin_network_helpers.h"

#include "Engine_classes.hpp"

// ---------------------------------------------------------------------------
// Global plugin self pointer (v19: single struct instead of 4 separate ptrs)
// ---------------------------------------------------------------------------
static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

// ---------------------------------------------------------------------------
// Plugin metadata
// ---------------------------------------------------------------------------
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
	"RuptureTimer",
	MODLOADER_BUILD_TAG,
	"Nhimself",
	"Tracks the rupture wave timer. Exports phase/countdown data to JSON for StreamDeck integration and optionally renders an in-game HUD overlay.",
	PLUGIN_INTERFACE_VERSION
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_worldReady = false;
static RuptureTimer::TimerState s_lastState{};

// Server-side: how often to broadcast the timer state to clients.
// Active phases (Warning/Burning/Cooling/Stabilizing) use the shorter interval
// because every second matters there; Stable can afford a longer heartbeat.
static constexpr float NET_SYNC_INTERVAL_STABLE = 10.0f;
static constexpr float NET_SYNC_INTERVAL_ACTIVE =  2.0f;
static float s_netSyncAccum = 0.0f;

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName)
{
	if (!world || !worldName) return;

	// Only track the main gameplay world
	if (std::string_view(worldName).find("ChimeraMain") == std::string_view::npos)
	{
		if (s_worldReady)
		{
			LOG_INFO("World changed to non-gameplay world (%s) — pausing rupture timer tracking", worldName);
			s_worldReady = false;
		}
		return;
	}

	LOG_INFO("World ready: %s — starting rupture timer tracking", worldName);
	s_worldReady = true;
	DataExport::EnsureOutputDir();
	DataExport::EnsureDiagnosticLogDir();
}

static void OnSaveLoaded()
{
	LOG_INFO("Save loaded — reading initial rupture timer state");
	s_lastState = RuptureTimer::ReadCurrentState();
	if (s_lastState.valid)
	{
		LOG_INFO("  Phase: %s | Remaining: %.1fs | Wave #%d | Type: %s",
			s_lastState.phaseName,
			s_lastState.phaseRemainingSeconds,
			s_lastState.waveNumber,
			s_lastState.waveTypeName);
		HudOverlay::SetState(s_lastState);
	}
	else
	{
		LOG_WARN("  Timer state not available yet (game state not fully loaded?)");
	}
}

static void OnEngineTick(float deltaSeconds)
{
	if (!s_worldReady) return;

	// Heartbeat log every 5s so we can confirm tick is firing and delta is valid
	static float s_logAccum = 0.0f;
	s_logAccum += deltaSeconds;
	if (s_logAccum >= 5.0f)
	{
		s_logAccum = 0.0f;
		LOG_DEBUG("Tick alive — deltaSeconds=%.4f", deltaSeconds);
	}

	s_lastState = RuptureTimer::ReadCurrentState();

	// Server: broadcast authoritative timer state to all clients.
	// Clients that lack UCrEnviroWaveSubsystem (dedicated server scenario) use
	// this packet instead of GetServerWorldTimeSeconds()-based estimation, which
	// is susceptible to clock-sync corrections causing visible HUD jumps.
	auto* hooks = g_self ? g_self->hooks : nullptr;
	if (hooks && hooks->Network && hooks->Network->IsServer() && s_lastState.valid)
	{
		bool activePhase = (s_lastState.phase != RuptureTimer::RupturePhase::Stable &&
		                    s_lastState.phase != RuptureTimer::RupturePhase::Unknown);
		float interval = activePhase ? NET_SYNC_INTERVAL_ACTIVE : NET_SYNC_INTERVAL_STABLE;

		s_netSyncAccum += deltaSeconds;
		if (s_netSyncAccum >= interval)
		{
			s_netSyncAccum = 0.0f;

			RuptureTimer::TimerSyncPacket pkt{};
			pkt.phaseRemainingSeconds = s_lastState.phaseRemainingSeconds;
			pkt.nextRuptureInSeconds  = s_lastState.nextRuptureInSeconds;
			pkt.stableRemaining       = s_lastState.stableRemaining;
			pkt.waveNumber            = s_lastState.waveNumber;
			pkt.phase                 = static_cast<uint8_t>(s_lastState.phase);
			pkt.waveType              = s_lastState.waveType;
			pkt.paused                = s_lastState.paused ? 1 : 0;
			// rawStage carries the server's EEnviroWaveStage so the client gets the
			// authoritative stage enum (Fadeout vs Growback) instead of guessing.
			pkt.rawStage              = (s_lastState.diag.rawStage >= 0)
			                              ? static_cast<uint8_t>(s_lastState.diag.rawStage)
			                              : 0;

			Network::SendPacketToAllClients(hooks, g_self, pkt);
			LOG_DEBUG("NetSync sent: phase=%d rem=%.1f nextRup=%.1f",
				(int)pkt.phase, pkt.phaseRemainingSeconds, pkt.nextRuptureInSeconds);
		}
	}

	DataExport::Update(deltaSeconds, s_lastState);
	DataExport::UpdateDiagnosticLog(deltaSeconds, s_lastState);
	HudOverlay::SetState(s_lastState);
}

// ---------------------------------------------------------------------------
// Plugin exports
// ---------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) PluginInfo* GetPluginInfo()
{
	return &s_pluginInfo;
}

__declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
	g_self = self;

	LOG_INFO("RuptureTimer initializing...");

	RuptureTimerConfig::Config::Initialize(self);

	if (!RuptureTimerConfig::Config::IsEnabled())
	{
		LOG_WARN("RuptureTimer is disabled in config");
		return true;
	}

	if (!self || !self->hooks)
	{
		LOG_ERROR("hooks interface is null — cannot register callbacks");
		return false;
	}

	auto* hooks = self->hooks;

	if (hooks->World)
	{
		hooks->World->RegisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
		hooks->World->RegisterOnSaveLoaded(OnSaveLoaded);
		LOG_DEBUG("Registered world/save callbacks");
	}

	if (hooks->Engine)
	{
		hooks->Engine->RegisterOnTick(OnEngineTick);
		LOG_DEBUG("Registered engine tick callback");
	}

	// Network sync — client registers a typed receive handler; server side sends
	// in OnEngineTick.  IsServer() check is deferred to tick time because the
	// network channel reports the correct side only after world init.
	if (hooks->Network)
	{
		Network::OnReceive<RuptureTimer::TimerSyncPacket>(
			hooks, self,
			[](const RuptureTimer::TimerSyncPacket& pkt)
			{
				RuptureTimer::ApplyNetworkSync(pkt);
			});
		LOG_DEBUG("Registered TimerSyncPacket receive handler");
	}
	else
	{
		LOG_DEBUG("hooks->Network not available — network sync disabled");
	}

	// HUD overlay — only register if the overlay is enabled in config.
	// hooks->HUD is null on server builds; Install() handles that gracefully.
	if (RuptureTimerConfig::Config::ShouldShowOverlay())
	{
		if (!HudOverlay::Install(hooks))
			LOG_WARN("HUD overlay could not be installed — in-game display will be unavailable");
	}
	else
	{
		LOG_DEBUG("HUD overlay disabled in config (HUD.ShowOverlay=false)");
	}

	// If we are being loaded/reloaded while a game world is already active
	// (e.g. via the mod loader UI), OnAnyWorldBeginPlay will not fire again —
	// it only fires on world transitions. Detect this by attempting a direct
	// state read: if it succeeds the game is already running and we can start
	// tracking immediately without waiting for the next world load.
	{
		s_lastState = RuptureTimer::ReadCurrentState();
		if (s_lastState.valid)
		{
			LOG_INFO("Active game world detected on init — starting tracking immediately");
			s_worldReady = true;
			DataExport::EnsureOutputDir();
			DataExport::EnsureDiagnosticLogDir();
			HudOverlay::SetState(s_lastState);
		}
	}

	LOG_INFO("RuptureTimer initialized — JSON output: %s | HUD overlay: %s",
		RuptureTimerConfig::Config::GetJsonFilePath(),
		RuptureTimerConfig::Config::ShouldShowOverlay() ? "enabled" : "disabled");

	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("RuptureTimer shutting down...");

	s_worldReady = false;

	if (g_self && g_self->hooks)
	{
		auto* hooks = g_self->hooks;

		HudOverlay::Remove(hooks);

		if (hooks->World)
		{
			hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
			hooks->World->UnregisterOnSaveLoaded(OnSaveLoaded);
		}
		if (hooks->Engine)
			hooks->Engine->UnregisterOnTick(OnEngineTick);
	}

	g_self = nullptr;
}

} // extern "C"
