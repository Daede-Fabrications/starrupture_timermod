#pragma once

#include <cstdint>
#include <functional>

// Forward declarations — avoid pulling in the full engine/interface headers.
namespace SDK { class UWorld; }
struct IPluginHooks;
struct IPluginSelf;

namespace RuptureTimer
{
	// Maps EEnviroWaveStage to user-friendly names
	// None=Stable, PreWave=Warning, Moving=Burning, Fadeout=Cooling, Growback=Stabilizing
	enum class RupturePhase : uint8_t
	{
		Unknown      = 0,
		Stable       = 1,   // Waiting for next rupture (EEnviroWaveStage::None)
		Warning      = 2,   // 15-second pre-wave warning (EEnviroWaveStage::PreWave)
		Burning      = 3,   // Wave moving across map - DANGEROUS (EEnviroWaveStage::Moving)
		Cooling      = 4,   // Aftermath / fadeout (EEnviroWaveStage::Fadeout)
		Stabilizing  = 5,   // Safe gathering window (EEnviroWaveStage::Growback)
	};

	// Raw diagnostic data captured during ReadCurrentState().
	// Used by the diagnostic log to record exactly what the game reported.
	struct DiagnosticRawData
	{
		const char*  codePath;              // "subsystem", "repActor", "stateMachine", "none"
		int          rawStage;              // EEnviroWaveStage as int (-1 if not available)
		int          rawWaveType;           // EEnviroWave as int (-1 if not available)
		float        rawNextTime;           // ACrWaveTimerActor::NextTime (absolute server timestamp)
		double       rawServerTime;         // GetServerWorldTimeSeconds()
		float        rawNextTimeRemaining;  // NextTime - serverTime, clamped >= 0
		int32_t      rawNextPhase;          // ACrWaveTimerActor::NextPhase
		bool         rawPaused;             // ACrWaveTimerActor::bPause
		bool         hasRepActor;           // was ACrGatherableSpawnersRepActor found?
		bool         hasSubsystem;          // was UCrEnviroWaveSubsystem found?
		float        rawProgress;           // GetCurrentStageProgress() (-1 if not available)
		// Hex dump of repActor memory at offsets 0x02A8..0x02B0 (8 bytes)
		// Allows detecting offset misalignment without recompiling.
		uint8_t      repActorBytes[8];      // raw bytes at repActor+0x02A8, zeroed if no repActor
		bool         repActorBytesValid;    // true if repActorBytes was populated

		// Game-internal phase name — what the engine actually reports, not our
		// interpretation.  Populated by all code paths:
		//   subsystem/repActor: EEnviroWaveStage name ("None","PreWave","Moving","Fadeout","Growback")
		//   stateMachine:       NextPhase label       ("NP0=Stable","NP1=PreWave","NP2=Moving","NP3=PostWave")
		//   netSync:            received RupturePhase  ("netSync:Stable", etc.)
		const char*  rawPhaseName;
	};

	struct TimerState
	{
		bool         valid;                  // false if world/game state not ready
		RupturePhase phase;
		const char*  phaseName;             // "Stable", "Warning", "Burning", "Cooling", "Stabilizing"
		float        phaseRemainingSeconds; // time left in the current phase (-1.0f = unknown)
		float        nextRuptureInSeconds;  // seconds until next Burning phase starts (the primary display value)
		                                    // 0 = currently in rupture; -1 = unknown
		int32_t      waveNumber;            // how many ruptures have completed (NextPhase)
		bool         paused;                // timer paused on server
		uint8_t      waveType;             // 0=None, 1=Heat, 2=Cold
		const char*  waveTypeName;         // "None", "Heat", "Cold"

		// Per-phase timing breakdown.
		// -1.0f = not available (dedicated server client) or not yet scheduled (future phases).
		// 0.0f  = this phase has already passed in the current cycle.
		// Populated in full mode (local / listen-server) only.
		float        warningRemaining;      // time left in Warning phase
		float        burningRemaining;      // time left in Burning phase
		float        coolingRemaining;      // time left in Cooling phase
		float        stabilizingRemaining;  // time left in Stabilizing phase
		float        stableRemaining;       // duration of the next Stable period

		DiagnosticRawData diag;             // raw values for diagnostic logging
	};

	// ---------------------------------------------------------------------------
	// Network sync — server plugin broadcasts this packet to clients every few
	// seconds so they get server-authoritative time without depending on
	// GetServerWorldTimeSeconds() drift/corrections.
	// Must be trivially copyable (plain POD, no pointers).
	// ---------------------------------------------------------------------------
	struct TimerSyncPacket
	{
		float    phaseRemainingSeconds;  // time left in current phase (-1 = unknown)
		float    nextRuptureInSeconds;   // primary countdown value (-1 = unknown)
		float    stableRemaining;        // stable-period duration (-1 = unknown)
		int32_t  waveNumber;            // ACrWaveTimerActor::NextPhase
		uint8_t  phase;                  // RupturePhase enum value
		uint8_t  waveType;               // 0=None 1=Heat 2=Cold
		uint8_t  paused;                 // 0/1
		uint8_t  rawStage;               // EEnviroWaveStage: 0=None 1=PreWave 2=Moving 3=Fadeout 4=Growback
	};
	// Size: 4+4+4+4+1+1+1+1 = 20 bytes, naturally aligned.

		// Returns true once ApplyNetworkSync() has been called at least once this
	// session.  Used by the client tick to permanently stop the local fallback
	// scan after the first server packet has been received.
	bool HasReceivedServerPacket();

	// Called from the Network::OnReceive handler in plugin.cpp when a broadcast arrives.
	// Stores the packet and returns a fully-populated TimerState built from it,
	// so the client never needs to call ReadCurrentState() after a packet arrives.
	TimerState ApplyNetworkSync(const TimerSyncPacket& pkt);

	// Read current rupture timer state from the game. Call only from game thread.
	// On client builds this should only be called when no server packet has ever
	// been received — once ApplyNetworkSync() has fired, use its return value instead.
	TimerState ReadCurrentState();

	// ---------------------------------------------------------------------------
	// Server-side broadcast — builds a TimerSyncPacket from the given state and
	// sends it to all connected clients.  Call once per broadcast interval from
	// the server engine tick.  No-ops if state is invalid or hooks->Network is null.
	// ---------------------------------------------------------------------------
	void BroadcastState(const TimerState& state, IPluginHooks* hooks, const IPluginSelf* self);

	// ---------------------------------------------------------------------------
	// Client-side receive registration — registers the OnReceive handler for
	// TimerSyncPacket.  The callback receives a fully-populated TimerState built
	// from the packet via ApplyNetworkSync(); no further processing needed by
	// the caller.  Call once from PluginInitClient.
	// ---------------------------------------------------------------------------
	void RegisterClientReceive(IPluginHooks* hooks, const IPluginSelf* self,
	   std::function<void(const TimerState&)> callback);

	// ---------------------------------------------------------------------------
	// World lifecycle — call from the plugin's world/experience hooks so that
	// ReadCurrentState() never has to scan GObjects on the hot tick path.
	//
	// OnWorldReady   : call from OnExperienceLoadComplete (server) or
	//          OnAnyWorldBeginPlay once the gameplay world is confirmed.
	//          Performs the one-time GObjects scan and caches the results.
	// OnWorldTeardown: call from OnBeforeWorldEndPlay (client) and from the
	//          non-gameplay branch of OnAnyWorldBeginPlay (server).
	//   Nulls all cached pointers so the tick path is safe.
	// ---------------------------------------------------------------------------
	void OnWorldReady();
	void OnWorldTeardown();
}
