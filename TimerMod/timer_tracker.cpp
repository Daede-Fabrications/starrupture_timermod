#include "timer_tracker.h"
#include "plugin_helpers.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "Basic.hpp"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"

namespace RuptureTimer
{

// ---------------------------------------------------------------------------
// Network sync state — written by ApplyNetworkSync() when a TimerSyncPacket
// arrives from the server plugin, read by ReadCurrentState() on every tick.
// ---------------------------------------------------------------------------
struct NetworkSyncState
{
	TimerSyncPacket pkt;
	ULONGLONG       receivedAtMs;  // GetTickCount64() at time of receipt
	bool            valid;
};
static NetworkSyncState s_netSync = {};

// Stale threshold: if no packet has arrived in this many seconds, fall back
// to the local computation paths (repActor / stateMachine).
static constexpr float NET_SYNC_STALE_SEC = 30.0f;

void ApplyNetworkSync(const TimerSyncPacket& pkt)
{
	s_netSync.pkt          = pkt;
	s_netSync.receivedAtMs = GetTickCount64();
	s_netSync.valid        = true;
	LOG_DEBUG("NetSync received: phase=%d rem=%.1f nextRup=%.1f waveType=%d",
		(int)pkt.phase, pkt.phaseRemainingSeconds,
		pkt.nextRuptureInSeconds, (int)pkt.waveType);
}

// ---------------------------------------------------------------------------
// Find UCrEnviroWaveSubsystem by iterating GObjects for the given world.
// World subsystems are UObjects whose Outer is the UWorld instance.
// ---------------------------------------------------------------------------
static SDK::UCrEnviroWaveSubsystem* FindEnviroWaveSubsystem(SDK::UWorld* world)
{
	SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
	if (!arr) return nullptr;

	for (int i = 0; i < arr->Num(); i++)
	{
		SDK::UObject* obj = arr->GetByIndex(i);
		if (!obj || !obj->Class || !obj->Outer) continue;
		if (obj->Outer != static_cast<SDK::UObject*>(world)) continue;
		if (obj->Class->GetName() == "CrEnviroWaveSubsystem")
			return static_cast<SDK::UCrEnviroWaveSubsystem*>(obj);
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Find ACrGatherableSpawnersRepActor — a replicated actor present on all
// clients (including dedicated server clients). It carries two Net/RepNotify
// fields that store the current wave type and stage, making it usable as a
// fallback phase source when UCrEnviroWaveSubsystem is absent.
// ---------------------------------------------------------------------------
static SDK::ACrGatherableSpawnersRepActor* FindGatherableSpawnersRepActor(SDK::UWorld* world)
{
	SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
	if (!arr) return nullptr;

	// ACrGatherableSpawnersRepActor is an AActor — Outer chain is:
	//   actor → ULevel → UWorld
	// So we must check two levels up, not one.
	for (int i = 0; i < arr->Num(); i++)
	{
		SDK::UObject* obj = arr->GetByIndex(i);
		if (!obj || !obj->Class) continue;
		if (!obj->Outer || obj->Outer->Outer != static_cast<SDK::UObject*>(world)) continue;
		if (obj->Class->GetName() == "CrGatherableSpawnersRepActor")
			return static_cast<SDK::ACrGatherableSpawnersRepActor*>(obj);
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Client-side phase state machine — dedicated server fallback.
//
// On a dedicated server client, UCrEnviroWaveSubsystem and
// ACrGatherableSpawnersRepActor are both absent. The only observable signals
// are NextTime (absolute server timestamp) and NextPhase (wave counter).
//
// Known fixed phase durations (empirically validated, full cycle = 54 min):
//   Burning:     30 s
//   Cooling:     60 s
//   Stabilizing: 600 s (10 min)
//   Stable:      2550 s (42 min 30 s)  ← derived: 3240 - 30 - 60 - 600
//
// Detectable transitions:
//   Stable → Burning:       nextTimeRemaining jumps from ~0 to ~BURNING_DURATION
//   Burning → Cooling:      nextTimeRemaining jumps from ~0 to a large value (full cycle ahead)
//   Cooling → Stabilizing:  elapsed time since Cooling start >= COOLING_DURATION
//   Stabilizing → Stable:   NextPhase increments (server broadcasts new wave number)
// ---------------------------------------------------------------------------
static constexpr float BURNING_DURATION     = 30.0f;
static constexpr float COOLING_DURATION     = 60.0f;
static constexpr float STABILIZING_DURATION = 600.0f;

struct ClientPhaseTracker
{
	RupturePhase phase                      = RupturePhase::Unknown;
	int32_t      prevNextPhase              = -1;
	SDK::UWorld* lastWorld                  = nullptr;
	bool         initialized                = false;
	float        lastObservedStableDuration = 2550.0f; // default canonical Stable duration
	float        phase3StartRemaining       = -1.0f;   // nextTimeRemaining when nextPhase first hit 3
};
static ClientPhaseTracker s_tracker;

// Update the state machine; call once per tick in the fallback path.
// nextPhase directly encodes the current interval (0=Stable, 1=Warning,
// 2=Burning, 3=post-wave). nextTimeRemaining = time left in that interval.
static void UpdateClientPhaseStateMachine(float /*serverTime*/, float nextTimeRemaining,
                                          int32_t nextPhase, SDK::UWorld* world)
{
	// Reset on world change, but carry over the last observed stable duration.
	if (world != s_tracker.lastWorld)
	{
		float savedStable = s_tracker.lastObservedStableDuration;
		s_tracker = ClientPhaseTracker{};
		s_tracker.lastWorld = world;
		s_tracker.lastObservedStableDuration = savedStable;
	}

	if (!s_tracker.initialized || nextPhase != s_tracker.prevNextPhase)
	{
		LOG_DEBUG("ClientPhase: nextPhase %d→%d remaining=%.1f",
			s_tracker.prevNextPhase, nextPhase, nextTimeRemaining);

		// Calibrate anchors on phase entry.
		// Only accept plausible stable durations (> 5 min) to guard against
		// clamped-zero values when NextTime hasn't been replicated yet.
		if (nextPhase == 0 && nextTimeRemaining > 300.0f)
			s_tracker.lastObservedStableDuration = nextTimeRemaining;
		if (nextPhase == 3)
			s_tracker.phase3StartRemaining = nextTimeRemaining;

		s_tracker.prevNextPhase = nextPhase;
		s_tracker.initialized   = true;
	}

	switch (nextPhase)
	{
		case 0:  s_tracker.phase = RupturePhase::Stable;  break;
		case 1:  s_tracker.phase = RupturePhase::Warning; break;
		case 2:  s_tracker.phase = RupturePhase::Burning; break;
		case 3:
		{
			// Phase 3 = Cooling (first COOLING_DURATION seconds) + Stabilizing (remainder).
			// coolingBoundary = nextTimeRemaining value at the Cooling→Stabilizing transition.
			float coolingBoundary = (s_tracker.phase3StartRemaining > 0.0f)
				? (s_tracker.phase3StartRemaining - COOLING_DURATION)
				: STABILIZING_DURATION;
			s_tracker.phase = (nextTimeRemaining > coolingBoundary)
				? RupturePhase::Cooling : RupturePhase::Stabilizing;
			break;
		}
		default: s_tracker.phase = RupturePhase::Stable; break;
	}
}

// ---------------------------------------------------------------------------
// Populate state fields from the client-side state machine result.
// nextTimeRemaining = time remaining in the current nextPhase interval.
// ---------------------------------------------------------------------------
static void FillStateFromStateMachine(TimerState& state, float nextTimeRemaining)
{
	switch (s_tracker.phase)
	{
		case RupturePhase::Stable:
			state.phase                 = RupturePhase::Stable;
			state.phaseName             = "Stable";
			state.phaseRemainingSeconds = nextTimeRemaining;
			state.nextRuptureInSeconds  = nextTimeRemaining;
			state.stableRemaining       = nextTimeRemaining;
			break;

		case RupturePhase::Warning:
			state.phase                 = RupturePhase::Warning;
			state.phaseName             = "Warning";
			state.phaseRemainingSeconds = nextTimeRemaining;
			state.nextRuptureInSeconds  = nextTimeRemaining;
			break;

		case RupturePhase::Burning:
			state.phase                 = RupturePhase::Burning;
			state.phaseName             = "Burning";
			state.phaseRemainingSeconds = nextTimeRemaining;
			state.nextRuptureInSeconds  = 0.0f;
			state.burningRemaining      = nextTimeRemaining;
			break;

		case RupturePhase::Cooling:
		{
			float coolingBoundary  = (s_tracker.phase3StartRemaining > 0.0f)
				? (s_tracker.phase3StartRemaining - COOLING_DURATION) : STABILIZING_DURATION;
			float coolingRemaining = nextTimeRemaining - coolingBoundary;
			if (coolingRemaining < 0.0f) coolingRemaining = 0.0f;
			state.phase                 = RupturePhase::Cooling;
			state.phaseName             = "Cooling";
			state.phaseRemainingSeconds = coolingRemaining;
			state.nextRuptureInSeconds  = nextTimeRemaining + s_tracker.lastObservedStableDuration;
			state.coolingRemaining      = coolingRemaining;
			state.stabilizingRemaining  = coolingBoundary;
			state.stableRemaining       = s_tracker.lastObservedStableDuration;
			break;
		}

		case RupturePhase::Stabilizing:
			state.phase                 = RupturePhase::Stabilizing;
			state.phaseName             = "Stabilizing";
			state.phaseRemainingSeconds = nextTimeRemaining;
			state.nextRuptureInSeconds  = nextTimeRemaining + s_tracker.lastObservedStableDuration;
			state.stabilizingRemaining  = nextTimeRemaining;
			state.stableRemaining       = s_tracker.lastObservedStableDuration;
			break;

		default:
			state.phase     = RupturePhase::Unknown;
			state.phaseName = "Unknown";
			break;
	}
}

// ---------------------------------------------------------------------------
// Compute the total duration of each stage from the game's own settings.
// These are the values the server/client agreed on — not hardcoded constants.
//
// FCrEnviroWaveSettings fields used per stage:
//   PreWave:     PreWaveDuration + WavePreWaveExplosionDuration
//   Moving:      |WaveEndPosition - WaveStartPosition| / WaveSpeed
//   Fadeout:     WaveFadeoutFireWaveDuration + WaveFadeoutBurningDuration + WaveFadeoutFadingDuration
//   Growback:    WaveGrowbackMoonPhaseDuration + WaveGrowbackRegrowthStartDuration + WaveGrowbackRegrowthDuration
// ---------------------------------------------------------------------------
static float StageDurationFromSettings(SDK::EEnviroWaveStage stage, const SDK::FCrEnviroWaveSettings& s)
{
	switch (stage)
	{
		case SDK::EEnviroWaveStage::PreWave:
			return s.PreWaveDuration + s.WavePreWaveExplosionDuration;

		case SDK::EEnviroWaveStage::Moving:
			if (s.WaveSpeed > 0.0f)
				return fabsf(s.WaveEndPosition - s.WaveStartPosition) / s.WaveSpeed;
			return 0.0f;

		case SDK::EEnviroWaveStage::Fadeout:
			return s.WaveFadeoutFireWaveDuration + s.WaveFadeoutBurningDuration + s.WaveFadeoutFadingDuration;

		case SDK::EEnviroWaveStage::Growback:
			return s.WaveGrowbackMoonPhaseDuration + s.WaveGrowbackRegrowthStartDuration + s.WaveGrowbackRegrowthDuration;

		default:
			return 0.0f;
	}
}

// ---------------------------------------------------------------------------
// Read current rupture timer state from game objects.
// Must be called from the game thread (engine tick callback).
// ---------------------------------------------------------------------------
TimerState ReadCurrentState()
{
	TimerState state{};
	state.valid    = false;
	state.phase    = RupturePhase::Unknown;
	state.phaseName = "Unknown";
	state.phaseRemainingSeconds = -1.0f;
	state.nextRuptureInSeconds  = -1.0f;
	state.waveNumber = 0;
	state.paused     = false;
	state.waveType   = 0;
	state.waveTypeName = "None";
	state.warningRemaining     = -1.0f;
	state.burningRemaining     = -1.0f;
	state.coolingRemaining     = -1.0f;
	state.stabilizingRemaining = -1.0f;
	state.stableRemaining      = -1.0f;

	// Initialize diagnostic raw data
	state.diag.codePath            = "none";
	state.diag.rawStage            = -1;
	state.diag.rawWaveType         = -1;
	state.diag.rawNextTime         = 0.0f;
	state.diag.rawServerTime       = 0.0;
	state.diag.rawNextTimeRemaining = 0.0f;
	state.diag.rawNextPhase        = -1;
	state.diag.rawPaused           = false;
	state.diag.hasRepActor         = false;
	state.diag.hasSubsystem        = false;
	state.diag.rawProgress         = -1.0f;
	memset(state.diag.repActorBytes, 0, sizeof(state.diag.repActorBytes));
	state.diag.repActorBytesValid  = false;
	state.diag.rawPhaseName        = "?";

	SDK::UWorld* world = SDK::UWorld::GetWorld();
	if (!world) { LOG_WARN_ONCE("ReadCurrentState: UWorld is null"); return state; }

	// Get game state for the replicated WaveTimerActor
	auto* gameState = static_cast<SDK::ACrGameStateBase*>(world->GameState);
	if (!gameState)         { LOG_WARN_ONCE("ReadCurrentState: GameState is null"); return state; }
	if (!gameState->WaveTimerActor) { LOG_WARN_ONCE("ReadCurrentState: WaveTimerActor is null (not yet replicated?)"); return state; }

	SDK::ACrWaveTimerActor* timerActor = gameState->WaveTimerActor;

	state.valid      = true;
	state.waveNumber = timerActor->NextPhase;
	state.paused     = timerActor->bPause;

	// NextTime is an absolute server timestamp. GetServerWorldTimeSeconds()
	// is the correct reference — it's the client-side estimate of the server's
	// world clock, which is what the server uses when setting NextTime.
	double serverTime = gameState->GetServerWorldTimeSeconds();
	float rawUnclamped = timerActor->NextTime - (float)serverTime;
	float nextTimeRemaining = (rawUnclamped < 0.0f) ? 0.0f : rawUnclamped;

	// Populate common diagnostic fields
	state.diag.rawNextTime          = timerActor->NextTime;
	state.diag.rawServerTime        = serverTime;
	state.diag.rawNextTimeRemaining = nextTimeRemaining;
	state.diag.rawNextPhase         = timerActor->NextPhase;
	state.diag.rawPaused            = timerActor->bPause;

	// If NextTime is significantly in the past the WaveTimerActor hasn't
	// received its replication update yet. Return Unknown until it does.
	if (rawUnclamped < -60.0f)
	{
		LOG_WARN_ONCE("ReadCurrentState: NextTime is %.0fs in the past — rupture cycle not active", rawUnclamped);
		state.phase     = RupturePhase::Unknown;
		state.phaseName = "Waiting";
		return state;
	}

	// Try to find the wave subsystem. It is server-authoritative and will be absent
	// on clients connected to a dedicated server (ShouldCreateSubsystem returns false
	// for client worlds). Fall back to timer-actor-only mode when unavailable.
	SDK::UCrEnviroWaveSubsystem* waveSub = FindEnviroWaveSubsystem(world);
	state.diag.hasSubsystem = (waveSub != nullptr);
	LOG_DEBUG("ReadCurrentState: waveSub=%s nextTimeRemaining=%.1f waveNumber=%d",
		waveSub ? "FOUND" : "absent", nextTimeRemaining, timerActor->NextPhase);

	if (!waveSub)
	{
		// ------------------------------------------------------------------
		// Network sync path — server plugin broadcasts authoritative state.
		// Preferred over repActor/stateMachine because the server computes
		// remaining time without GetServerWorldTimeSeconds() drift.
		// We interpolate locally using wall-clock elapsed time since receipt.
		// ------------------------------------------------------------------
		if (s_netSync.valid)
		{
			ULONGLONG ageMs = GetTickCount64() - s_netSync.receivedAtMs;
			if (ageMs < static_cast<ULONGLONG>(NET_SYNC_STALE_SEC * 1000.0f))
			{
				const TimerSyncPacket& p = s_netSync.pkt;
				float elapsed = static_cast<float>(ageMs) / 1000.0f;

				state.diag.codePath  = "netSync";
				state.diag.rawStage  = static_cast<int>(p.rawStage);
				state.phase    = static_cast<RupturePhase>(p.phase);
				state.waveType = p.waveType;
				state.paused   = (p.paused != 0);
				state.waveNumber = p.waveNumber;

				// rawPhaseName from the server's authoritative EEnviroWaveStage
				switch (p.rawStage)
				{
					case 0:  state.diag.rawPhaseName = "None";     break; // EEnviroWaveStage::None
					case 1:  state.diag.rawPhaseName = "PreWave";  break;
					case 2:  state.diag.rawPhaseName = "Moving";   break;
					case 3:  state.diag.rawPhaseName = "Fadeout";  break;
					case 4:  state.diag.rawPhaseName = "Growback"; break;
					default: state.diag.rawPhaseName = "Stage?";   break;
				}

				switch (state.phase)
				{
					case RupturePhase::Stable:      state.phaseName = "Stable";      break;
					case RupturePhase::Warning:     state.phaseName = "Warning";     break;
					case RupturePhase::Burning:     state.phaseName = "Burning";     break;
					case RupturePhase::Cooling:     state.phaseName = "Cooling";     break;
					case RupturePhase::Stabilizing: state.phaseName = "Stabilizing"; break;
					default:                        state.phaseName = "Unknown";     break;
				}
				switch (state.waveType)
				{
					case 1:  state.waveTypeName = "Heat"; break;
					case 2:  state.waveTypeName = "Cold"; break;
					default: state.waveTypeName = "None"; break;
				}

				auto Interp = [elapsed](float base) -> float {
					return (base >= 0.0f) ? fmaxf(0.0f, base - elapsed) : -1.0f;
				};
				state.phaseRemainingSeconds = Interp(p.phaseRemainingSeconds);
				state.nextRuptureInSeconds  = Interp(p.nextRuptureInSeconds);
				state.stableRemaining       = Interp(p.stableRemaining);

				return state;
			}
			// Packet is stale — fall through to repActor / stateMachine
			LOG_DEBUG("ReadCurrentState: netSync packet is stale (%.0fs) — falling back",
				static_cast<float>(ageMs) / 1000.0f);
		}

		LOG_DEBUG("ReadCurrentState: UCrEnviroWaveSubsystem absent — using replication-actor mode");

		// Try ACrGatherableSpawnersRepActor — a replicated actor present on all clients.
		// It holds RepEnviroWaveTypeChange and RepEnviroWaveStageChange as persistent
		// Net/RepNotify fields, giving us accurate phase and wave-type labels.
		// Per-phase breakdown timers (-1) are not available without the subsystem.
		SDK::ACrGatherableSpawnersRepActor* repActor = FindGatherableSpawnersRepActor(world);
		if (repActor)
		{
			state.diag.codePath    = "repActor";
			state.diag.hasRepActor = true;

			// Capture raw bytes at repActor+0x02A8 (8 bytes). Expected layout per SDK:
			//   [0..3] RepGlobalGatherablePCGSeed (int32 LE)
			//   [4]    RepEnviroWaveTypeChange     (uint8: 0=None,1=Heat,2=Cold)
			//   [5]    RepEnviroWaveStageChange    (uint8: 0=None,1=PreWave,2=Moving,3=Fadeout,4=Growback)
			//   [6..7] padding
			// If byte[5] != rawStage, the SDK offsets are stale → file bug at
			// https://github.com/AlienXAXS/StarRupture-Game-SDK
			const uint8_t* base = reinterpret_cast<const uint8_t*>(repActor);
			memcpy(state.diag.repActorBytes, base + 0x02A8, 8);
			state.diag.repActorBytesValid = true;

			SDK::EEnviroWaveStage repStage = repActor->RepEnviroWaveStageChange;
			SDK::EEnviroWave      repWave  = repActor->RepEnviroWaveTypeChange;

			state.diag.rawStage    = static_cast<int>(repStage);
			state.diag.rawWaveType = static_cast<int>(repWave);
			switch (repStage)
			{
				case SDK::EEnviroWaveStage::None:     state.diag.rawPhaseName = "None";     break;
				case SDK::EEnviroWaveStage::PreWave:  state.diag.rawPhaseName = "PreWave";  break;
				case SDK::EEnviroWaveStage::Moving:   state.diag.rawPhaseName = "Moving";   break;
				case SDK::EEnviroWaveStage::Fadeout:  state.diag.rawPhaseName = "Fadeout";  break;
				case SDK::EEnviroWaveStage::Growback: state.diag.rawPhaseName = "Growback"; break;
				default:                              state.diag.rawPhaseName = "Stage?";   break;
			}

			LOG_DEBUG("ReadCurrentState: repActor FOUND — repStage=%d repWave=%d",
				static_cast<int>(repStage), static_cast<int>(repWave));

			state.waveType = static_cast<uint8_t>(repWave);
			switch (repWave)
			{
				case SDK::EEnviroWave::Heat: state.waveTypeName = "Heat"; break;
				case SDK::EEnviroWave::Cold: state.waveTypeName = "Cold"; break;
				default:                     state.waveTypeName = "None"; break;
			}

			switch (repStage)
			{
				case SDK::EEnviroWaveStage::None:
					// Stable — NextTime is the absolute time the next wave fires.
					state.phase                 = RupturePhase::Stable;
					state.phaseName             = "Stable";
					state.phaseRemainingSeconds = nextTimeRemaining;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					state.stableRemaining       = nextTimeRemaining;
					break;

				case SDK::EEnviroWaveStage::PreWave:
					// Warning — NextTime appears to point to when burning starts.
					state.phase                 = RupturePhase::Warning;
					state.phaseName             = "Warning";
					state.phaseRemainingSeconds = nextTimeRemaining;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					break;

				case SDK::EEnviroWaveStage::Moving:
					// Burning — wave is active now.
					state.phase                 = RupturePhase::Burning;
					state.phaseName             = "Burning";
					state.phaseRemainingSeconds = -1.0f; // no progress without subsystem
					state.nextRuptureInSeconds  = 0.0f;
					break;

				case SDK::EEnviroWaveStage::Fadeout:
					// Cooling — NextTime is the stable-period countdown (undercount of
					// total next_rupture since we can't add remaining cooling time).
					state.phase                 = RupturePhase::Cooling;
					state.phaseName             = "Cooling";
					state.phaseRemainingSeconds = -1.0f;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					state.stableRemaining       = nextTimeRemaining;
					break;

				case SDK::EEnviroWaveStage::Growback:
					// Stabilizing — same situation as Cooling.
					state.phase                 = RupturePhase::Stabilizing;
					state.phaseName             = "Stabilizing";
					state.phaseRemainingSeconds = -1.0f;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					state.stableRemaining       = nextTimeRemaining;
					break;

				default:
					state.phase     = RupturePhase::Unknown;
					state.phaseName = "Unknown";
					break;
			}
		}
		else
		{
			// Last-resort fallback: no subsystem and no rep actor found.
			// NextPhase directly encodes the current interval (0=Stable, 1=Warning,
			// 2=Burning, 3=post-wave). nextTimeRemaining = time left in that interval.
			state.diag.codePath = "stateMachine";
			switch (timerActor->NextPhase)
			{
				case 0:  state.diag.rawPhaseName = "NP0=Stable";   break;
				case 1:  state.diag.rawPhaseName = "NP1=PreWave";  break;
				case 2:  state.diag.rawPhaseName = "NP2=Moving";   break;
				case 3:  state.diag.rawPhaseName = "NP3=PostWave"; break;
				default: state.diag.rawPhaseName = "NP?=Unknown";  break;
			}
			LOG_DEBUG("ReadCurrentState: repActor absent — using client-side phase state machine");

			UpdateClientPhaseStateMachine(
				(float)serverTime, nextTimeRemaining,
				timerActor->NextPhase, world);

			FillStateFromStateMachine(state, nextTimeRemaining);
		}
		return state;
	}

	// --- Full mode: subsystem is available (local / listen server) ---
	state.diag.codePath = "subsystem";

	// Wave type (Heat / Cold)
	SDK::EEnviroWave waveType = waveSub->GetCurrentType();
	state.waveType = static_cast<uint8_t>(waveType);
	switch (waveType)
	{
		case SDK::EEnviroWave::Heat: state.waveTypeName = "Heat"; break;
		case SDK::EEnviroWave::Cold: state.waveTypeName = "Cold"; break;
		default:                     state.waveTypeName = "None"; break;
	}

	SDK::EEnviroWaveStage stage = waveSub->GetCurrentStage();
	state.diag.rawStage    = static_cast<int>(stage);
	state.diag.rawWaveType = static_cast<int>(waveType);
	switch (stage)
	{
		case SDK::EEnviroWaveStage::None:     state.diag.rawPhaseName = "None";     break;
		case SDK::EEnviroWaveStage::PreWave:  state.diag.rawPhaseName = "PreWave";  break;
		case SDK::EEnviroWaveStage::Moving:   state.diag.rawPhaseName = "Moving";   break;
		case SDK::EEnviroWaveStage::Fadeout:  state.diag.rawPhaseName = "Fadeout";  break;
		case SDK::EEnviroWaveStage::Growback: state.diag.rawPhaseName = "Growback"; break;
		default:                              state.diag.rawPhaseName = "Stage?";   break;
	}
	LOG_DEBUG("ReadCurrentState: subsystem stage=%d waveType=%d nextTimeRemaining=%.1f",
		static_cast<int>(stage), static_cast<int>(waveType), nextTimeRemaining);

	if (stage == SDK::EEnviroWaveStage::None)
	{
		// Stable period — NextTime is the absolute server time when the next wave fires
		state.phase     = RupturePhase::Stable;
		state.phaseName = "Stable";
		state.phaseRemainingSeconds = nextTimeRemaining;
		state.nextRuptureInSeconds  = nextTimeRemaining;
		state.stableRemaining       = nextTimeRemaining;
		// Wave-type and per-wave-phase durations are not meaningful until the next wave starts.
		return state;
	}

	// For all active wave stages, fetch the actual durations from the game client
	SDK::FCrEnviroWaveSettings settings = waveSub->GetCurrentStageSettings();

	float progress = waveSub->GetCurrentStageProgress();
	state.diag.rawProgress = progress;
	if (progress < 0.0f) progress = 0.0f;
	if (progress > 1.0f) progress = 1.0f;

	float stageDuration = StageDurationFromSettings(stage, settings);
	float phaseRemaining = stageDuration * (1.0f - progress);
	if (phaseRemaining < 0.0f) phaseRemaining = 0.0f;

	// During Cooling/Stabilizing, the server has already set the next wave's absolute time.
	// nextTimeRemaining gives the Stable period before the NEXT rupture.
	float stableCountdown = nextTimeRemaining;

	// Precompute full durations of adjacent phases for the breakdown timers.
	float fullBurning     = StageDurationFromSettings(SDK::EEnviroWaveStage::Moving,   settings);
	float fullCooling     = StageDurationFromSettings(SDK::EEnviroWaveStage::Fadeout,  settings);
	float fullStabilizing = StageDurationFromSettings(SDK::EEnviroWaveStage::Growback, settings);

	switch (stage)
	{
		case SDK::EEnviroWaveStage::PreWave:
			state.phase     = RupturePhase::Warning;
			state.phaseName = "Warning";
			state.phaseRemainingSeconds = phaseRemaining;
			// Warning → Burning is immediate after this phase
			state.nextRuptureInSeconds  = phaseRemaining;
			// Breakdown: warning is live; downstream phases have not started yet
			state.warningRemaining      = phaseRemaining;
			state.burningRemaining      = fullBurning;
			state.coolingRemaining      = fullCooling;
			state.stabilizingRemaining  = fullStabilizing;
			// stableRemaining unknown — next stable period not yet scheduled
			break;

		case SDK::EEnviroWaveStage::Moving:
			state.phase     = RupturePhase::Burning;
			state.phaseName = "Burning";
			state.phaseRemainingSeconds = phaseRemaining;
			// Currently in rupture — next rupture starts after the full next cycle
			state.nextRuptureInSeconds  = 0.0f;
			// Breakdown
			state.warningRemaining      = 0.0f;
			state.burningRemaining      = phaseRemaining;
			state.coolingRemaining      = fullCooling;
			state.stabilizingRemaining  = fullStabilizing;
			// stableRemaining unknown — not yet scheduled during Burning
			break;

		case SDK::EEnviroWaveStage::Fadeout:
		{
			state.phase     = RupturePhase::Cooling;
			state.phaseName = "Cooling";
			state.phaseRemainingSeconds = phaseRemaining;
			// Next rupture = rest of Cooling + full Stabilizing + Stable countdown
			state.nextRuptureInSeconds  = phaseRemaining + fullStabilizing + stableCountdown;
			// Breakdown
			state.warningRemaining      = 0.0f;
			state.burningRemaining      = 0.0f;
			state.coolingRemaining      = phaseRemaining;
			state.stabilizingRemaining  = fullStabilizing;
			state.stableRemaining       = stableCountdown;
			break;
		}

		case SDK::EEnviroWaveStage::Growback:
			state.phase     = RupturePhase::Stabilizing;
			state.phaseName = "Stabilizing";
			state.phaseRemainingSeconds = phaseRemaining;
			// Next rupture = rest of Stabilizing + Stable countdown
			state.nextRuptureInSeconds  = phaseRemaining + stableCountdown;
			// Breakdown
			state.warningRemaining      = 0.0f;
			state.burningRemaining      = 0.0f;
			state.coolingRemaining      = 0.0f;
			state.stabilizingRemaining  = phaseRemaining;
			state.stableRemaining       = stableCountdown;
			break;

		default:
			state.phase     = RupturePhase::Unknown;
			state.phaseName = "Unknown";
			break;
	}

	return state;
}

} // namespace RuptureTimer
