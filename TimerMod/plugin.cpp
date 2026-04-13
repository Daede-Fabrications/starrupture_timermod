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

// How often to broadcast.  Active phases use the short interval because every
// second matters; Stable can afford a longer heartbeat.
static constexpr float NET_SYNC_INTERVAL_STABLE = 10.0f;
static constexpr float NET_SYNC_INTERVAL_ACTIVE  =  2.0f;
static float s_netSyncAccum = 0.0f;

// ---------------------------------------------------------------------------
// Engine tick
//
// Every TimerMod instance starts as a potential broadcaster.  The instance
// whose serverId is numerically lowest (i.e. was initialised earliest, since
// the ID is seeded from GetTickCount64) wins the election and keeps sending.
// All other instances defer once they receive a broadcast from the winner.
//
// Broadcasting is suppressed as soon as IsDeferredToExternalBroadcaster()
// returns true, so the elected winner is the only one sending packets.
// ---------------------------------------------------------------------------
static void OnEngineTick(float deltaSeconds)
{
	if (!s_worldReady) return;

	// Heartbeat log every 5 s to confirm tick is alive
	static float s_logAccum = 0.0f;
	s_logAccum += deltaSeconds;
	if (s_logAccum >= 5.0f)
	{
		s_logAccum = 0.0f;
		LOG_DEBUG("Tick alive — deltaSeconds=%.4f | broadcaster=%s",
			deltaSeconds,
			RuptureTimer::IsDeferredToExternalBroadcaster() ? "DEFERRED" : "SELF");
	}

	s_lastState = RuptureTimer::ReadCurrentState();

	// ---- Broadcast path ------------------------------------------------
	// Only the elected broadcaster (lowest serverId, not yet deferred) sends.
	auto* hooks = g_self ? g_self->hooks : nullptr;
	if (hooks && hooks->Network && s_lastState.valid
	    && !RuptureTimer::IsDeferredToExternalBroadcaster())
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
			pkt.serverId              = RuptureTimer::GetServerId();
			pkt.phase                 = static_cast<uint8_t>(s_lastState.phase);
			pkt.waveType              = s_lastState.waveType;
			pkt.paused                = s_lastState.paused ? 1 : 0;
			pkt.rawStage              = (s_lastState.diag.rawStage >= 0)
			                              ? static_cast<uint8_t>(s_lastState.diag.rawStage)
			                              : 0;

			Network::SendPacketToAllClients(hooks, g_self, pkt);
			LOG_DEBUG("NetSync broadcast: phase=%d rem=%.1f nextRup=%.1f id=0x%08X",
				(int)pkt.phase, pkt.phaseRemainingSeconds,
				pkt.nextRuptureInSeconds, pkt.serverId);
		}
	}

	DataExport::Update(deltaSeconds, s_lastState);
	DataExport::UpdateDiagnosticLog(deltaSeconds, s_lastState);
	HudOverlay::SetState(s_lastState);
}

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

static void OnExperienceLoadComplete()
{
	LOG_INFO("Experience load complete — reading initial rupture timer state");
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
		LOG_WARN("  Timer state not available yet after experience load — will retry on next tick");
	}
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
		hooks->World->RegisterOnExperienceLoadComplete(OnExperienceLoadComplete);
		LOG_DEBUG("Registered world/save callbacks");
	}

	// ---------------------------------------------------------------------------
	// Network — broadcaster election
	//
	// Every TimerMod instance generates its own serverId and starts as a candidate
	// broadcaster.  When two instances are present, the one with the lower serverId
	// (earlier start time) wins: the other instance defers once it receives the
	// winner's first broadcast packet.
	//
	// No configuration required — the same DLL handles both roles.
	// ---------------------------------------------------------------------------
	RuptureTimer::InitServerMode();  // generates serverId for this instance

	if (hooks->Network)
	{
		// Register receive handler on every instance.
		// ApplyNetworkSync() runs the election: lower serverId wins, this instance
		// defers and stops broadcasting; higher serverId is discarded.
		Network::OnReceive<RuptureTimer::TimerSyncPacket>(
			hooks, self,
			[](const RuptureTimer::TimerSyncPacket& pkt)
			{
				RuptureTimer::ApplyNetworkSync(pkt);
			});
		LOG_INFO("Network ready — serverId=0x%08X | broadcasting until a lower-ID instance is seen",
			RuptureTimer::GetServerId());
	}
	else
	{
		LOG_INFO("Network: hooks->Network not available — local-only mode, serverId=0x%08X",
			RuptureTimer::GetServerId());
	}

	if (hooks->Engine)
	{
		hooks->Engine->RegisterOnTick(OnEngineTick);
		LOG_DEBUG("Registered OnEngineTick");
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
	// it only fires on world transitions.  Detect this by attempting a direct
	// state read.
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
			hooks->World->UnregisterOnExperienceLoadComplete(OnExperienceLoadComplete);
		}
		if (hooks->Engine)
			hooks->Engine->UnregisterOnTick(OnEngineTick);
	}

	g_self = nullptr;
}

} // extern "C"
