#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "timer_tracker.h"
#include "data_export.h"
#include "hud_overlay.h"

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

// Server broadcasts state to all clients once per second.
// The client's HUD interpolates smoothly between packets using its own
// QPC-based countdown — no sub-interval throttling needed server-side.
static constexpr float NET_SYNC_INTERVAL = 1.0f;
static float s_netSyncAccum = 0.0f;

// ---------------------------------------------------------------------------
// Server engine tick
// ---------------------------------------------------------------------------
static void OnEngineTickServer(float deltaSeconds)
{
	if (!s_worldReady) return;

	s_netSyncAccum += deltaSeconds;
	if (s_netSyncAccum < NET_SYNC_INTERVAL)
		return;
	s_netSyncAccum = 0.0f;

	s_lastState = RuptureTimer::ReadCurrentState();

	auto* hooks = g_self ? g_self->hooks : nullptr;
	RuptureTimer::BroadcastState(s_lastState, hooks, g_self);

	DataExport::Update(NET_SYNC_INTERVAL, s_lastState);
	DataExport::UpdateDiagnosticLog(NET_SYNC_INTERVAL, s_lastState);
}

// ---------------------------------------------------------------------------
// Client engine tick
//
// Before the first server packet: runs local ReadCurrentState() and updates
// the HUD so the display isn't blank while waiting.
// After the first server packet: local lookups are permanently disabled.
// The OnReceive handler is the sole updater of s_lastState and the HUD.
// DataExport still runs every tick for the JSON output.
// ---------------------------------------------------------------------------
static void OnEngineTickClient(float deltaSeconds)
{
	if (!s_worldReady) return;

	if (!RuptureTimer::HasReceivedServerPacket())
	{
		// No server packet yet — use local data and push it to the HUD.
		s_lastState = RuptureTimer::ReadCurrentState();
		HudOverlay::SetState(s_lastState);
	}
	// else: OnReceive handles s_lastState and HudOverlay::SetState — nothing to do here.

	DataExport::Update(deltaSeconds, s_lastState);
	DataExport::UpdateDiagnosticLog(deltaSeconds, s_lastState);
}

// ---------------------------------------------------------------------------
// Shared world/experience callbacks
// ---------------------------------------------------------------------------

static void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName)
{
	if (!world || !worldName) return;

	if (std::string_view(worldName).find("ChimeraMain") == std::string_view::npos)
	{
		if (s_worldReady)
		{
			LOG_INFO("World changed to non-gameplay world (%s) — pausing rupture timer tracking", worldName);
			s_worldReady = false;
			RuptureTimer::OnWorldTeardown();
		}
		return;
	}

	LOG_INFO("World ready: %s — starting rupture timer tracking", worldName);
	s_worldReady = true;
	DataExport::EnsureOutputDir();
	DataExport::EnsureDiagnosticLogDir();

	// Cache world objects immediately so ReadCurrentState() has valid pointers
	// from the very first tick.  OnExperienceLoadComplete may fire before or
	// after this callback depending on the connection type; calling OnWorldReady()
	// here ensures the cache is always populated regardless of ordering.
	// OnExperienceLoadComplete will call it again (idempotent — just overwrites
	// the pointers with the same values once actors are fully replicated).
	RuptureTimer::OnWorldReady();
}

// Server only: reads an initial state snapshot so the first broadcast is
// populated before the first full tick interval elapses.
static void OnExperienceLoadCompleteServer()
{
	// ExperienceLoadComplete fires for every world the server loads, including
	// the pre-game config map.  Only proceed if OnAnyWorldBeginPlay has already
	// confirmed this is a ChimeraMain world.
	if (!s_worldReady)
	{
		LOG_DEBUG("Experience load complete — world not ChimeraMain, skipping tracker init");
		return;
	}

	LOG_INFO("Experience load complete — reading initial rupture timer state (server)");
	RuptureTimer::OnWorldReady();

	s_lastState = RuptureTimer::ReadCurrentState();
	if (s_lastState.valid)
	{
		LOG_INFO("  Phase: %s | Remaining: %.1fs | Wave #%d | Type: %s",
			s_lastState.phaseName,
			s_lastState.phaseRemainingSeconds,
			s_lastState.waveNumber,
			s_lastState.waveTypeName);
	}
	else
	{
		LOG_WARN("  Timer state not available yet after experience load — will retry on first tick");
	}
}

// Client only: the state arrives via network packets; this callback is used
// only to push any already-received state to the HUD immediately on load.
static void OnExperienceLoadCompleteClient()
{
	// Guard the same as the server path — don't scan a non-gameplay world.
	if (!s_worldReady)
	{
		LOG_DEBUG("Experience load complete — world not ChimeraMain, skipping tracker init");
		return;
	}

	LOG_INFO("Experience load complete (client) — scanning for local objects, HUD will update on next packet");
	RuptureTimer::OnWorldReady();

	if (s_lastState.valid)
		HudOverlay::SetState(s_lastState);
}

// Client only: fired before the gameplay world tears down.
// Clears HUD display state and marks the world as not ready so the tick
// callbacks don't try to render stale data during level unload.
static void OnBeforeWorldEndPlayClient(SDK::UWorld* /*world*/, const char* worldName)
{
	LOG_INFO("World ending (%s) — resetting client HUD and timer state", worldName ? worldName : "?");
	s_worldReady = false;
	s_lastState = {};
	RuptureTimer::OnWorldTeardown();
	HudOverlay::Reset();
}

// ---------------------------------------------------------------------------
// PluginInitServer
//
// Registers everything the server side needs:
//   - World / experience callbacks
//   - Engine tick for state reading + broadcasting
//   - Data export (so server-side JSON / diagnostic log works)
// ---------------------------------------------------------------------------
static bool PluginInitServer(IPluginSelf* self, IPluginHooks* hooks)
{
	LOG_INFO("RuptureTimer: initializing as SERVER");

	if (hooks->World)
	{
		hooks->World->RegisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
		hooks->World->RegisterOnExperienceLoadComplete(OnExperienceLoadCompleteServer);
		LOG_DEBUG("Server: registered world/experience callbacks");
	}

	if (hooks->Engine)
	{
		hooks->Engine->RegisterOnTick(OnEngineTickServer);
		LOG_DEBUG("Server: registered OnEngineTickServer");
	}

	if (hooks->Network)
		LOG_INFO("Server: network ready — will broadcast TimerSyncPacket to clients");
	else
		LOG_WARN("Server: hooks->Network is null — clients will not receive timer sync");

	// Detect an already-active world (hot reload via mod loader UI).
	// Only treat the world as ready if it's actually ChimeraMain — the server
	// may be sitting on its config/lobby map when the plugin is first loaded.
	if (SDK::UWorld* world = SDK::UWorld::GetWorld())
	{
		std::string worldName = world->GetName();
		if (worldName.find("ChimeraMain") != std::string::npos)
		{
			LOG_INFO("Server: active ChimeraMain world detected on init — starting tracking immediately");
			s_worldReady = true;
			RuptureTimer::OnWorldReady();
			s_lastState = RuptureTimer::ReadCurrentState();
			DataExport::EnsureOutputDir();
			DataExport::EnsureDiagnosticLogDir();
		}
		else
		{
			LOG_DEBUG("Server: active world '%s' is not ChimeraMain — deferring tracker init", worldName.c_str());
		}
	}

	LOG_INFO("Server: initialized — JSON output: %s", RuptureTimerConfig::Config::GetJsonFilePath());
	return true;
}

// ---------------------------------------------------------------------------
// PluginInitClient
//
// Registers everything the client side needs:
//   - Network receive handler (feeds s_lastState via ApplyNetworkSync)
//   - World / experience callbacks
//   - Engine tick for HUD + data export
//   - HUD overlay (if enabled in config)
// ---------------------------------------------------------------------------
static bool PluginInitClient(IPluginSelf* self, IPluginHooks* hooks)
{
	LOG_INFO("RuptureTimer: initializing as CLIENT");

	if (hooks->Network)
	{
		RuptureTimer::RegisterClientReceive(hooks, self,
			[](const RuptureTimer::TimerState& state)
			{
				s_lastState = state;
				HudOverlay::SetState(s_lastState);
			});
		LOG_INFO("Client: network ready — listening for TimerSyncPacket broadcasts");
	}
	else
	{
		LOG_WARN("Client: hooks->Network is null — timer sync from server unavailable");
	}

	if (hooks->World)
	{
		hooks->World->RegisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
		hooks->World->RegisterOnExperienceLoadComplete(OnExperienceLoadCompleteClient);
		hooks->World->RegisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlayClient);
		LOG_DEBUG("Client: registered world/experience callbacks");
	}

	if (hooks->Engine)
	{
		hooks->Engine->RegisterOnTick(OnEngineTickClient);
		LOG_DEBUG("Client: registered OnEngineTickClient");
	}

	// HUD overlay — client only; hooks->HUD is null on server builds.
	if (RuptureTimerConfig::Config::ShouldShowOverlay())
	{
		if (!HudOverlay::Install(hooks))
			LOG_WARN("Client: HUD overlay could not be installed — in-game display unavailable");
	}
	else
	{
		LOG_DEBUG("Client: HUD overlay disabled in config (HUD.ShowOverlay=false)");
	}

	// No active-world hot-reload read here: the client has no authoritative
	// subsystem to read from.  The next server broadcast will populate state.

	LOG_INFO("Client: initialized — HUD overlay: %s",
		RuptureTimerConfig::Config::ShouldShowOverlay() ? "enabled" : "disabled");
	return true;
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

		if (!hooks->Network)
		{
			// Generic / offline build — no network channel available.
			// Run server-style init so local state reading and data export still work.
			LOG_INFO("hooks->Network is null — running in offline/local mode");
			return PluginInitServer(self, hooks);
		}

		if (hooks->Network->IsServer())
			return PluginInitServer(self, hooks);

		return PluginInitClient(self, hooks);
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("RuptureTimer shutting down...");

		s_worldReady = false;

		if (g_self && g_self->hooks)
		{
			auto* hooks = g_self->hooks;

			if (hooks->Network && !hooks->Network->IsServer())
			{
				// Client teardown
				HudOverlay::Remove(hooks);

				if (hooks->World)
				{
					hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
					hooks->World->UnregisterOnExperienceLoadComplete(OnExperienceLoadCompleteClient);
					hooks->World->UnregisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlayClient);
				}
				if (hooks->Engine)
					hooks->Engine->UnregisterOnTick(OnEngineTickClient);
			}
			else
			{
				// Server / offline teardown
				if (hooks->World)
				{
					hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
					hooks->World->UnregisterOnExperienceLoadComplete(OnExperienceLoadCompleteServer);
				}
				if (hooks->Engine)
					hooks->Engine->UnregisterOnTick(OnEngineTickServer);
			}
		}

		g_self = nullptr;
	}

} // extern "C"
