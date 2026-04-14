#include "timer_tracker.h"
#include "plugin_helpers.h"
#include "plugin_network_helpers.h"

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
		bool         valid;
	};
	static NetworkSyncState s_netSync = {};

	// Stale threshold: age at which a fresh packet is no longer considered live.
	// Once a client has received at least one packet this session it stays in
	// server-authoritative mode — showing frozen last-known data instead of
	// falling back to local scanning (codePath = "netSync-stale").
	static constexpr float NET_SYNC_STALE_SEC = 30.0f;

	// Client-side: set true on first successful packet receipt, never cleared.
	// Once true, ReadCurrentState() stays in server-authoritative mode.
	static bool s_everReceivedServerPacket = false;

	bool HasReceivedServerPacket()
	{
		return s_everReceivedServerPacket;
	}

	TimerState ApplyNetworkSync(const TimerSyncPacket& pkt)
	{
		s_netSync.pkt = pkt;
		s_netSync.receivedAtMs = GetTickCount64();
		s_netSync.valid = true;
		s_everReceivedServerPacket = true;

		LOG_DEBUG("NetSync received: phase=%d rem=%.1f nextRup=%.1f waveType=%d",
			static_cast<int>(pkt.phase), pkt.phaseRemainingSeconds,
			pkt.nextRuptureInSeconds, static_cast<int>(pkt.waveType));

		// Build a TimerState directly from the packet so the client never has to
		// call ReadCurrentState() (which would trigger a full GObjects scan that
		// serves no purpose when authoritative data has already arrived).
		TimerState state{};
		state.valid = true;
		state.waveNumber = pkt.waveNumber;
		state.paused = (pkt.paused != 0);
		state.waveType = pkt.waveType;
		state.phase = static_cast<RupturePhase>(pkt.phase);

		switch (state.phase)
		{
		case RupturePhase::Stable:      state.phaseName = "Stable";      break;
		case RupturePhase::Warning:     state.phaseName = "Warning";     break;
		case RupturePhase::Burning:     state.phaseName = "Burning";     break;
		case RupturePhase::Cooling:     state.phaseName = "Cooling";     break;
		case RupturePhase::Stabilizing: state.phaseName = "Stabilizing"; break;
		default:             state.phaseName = "Unknown";     break;
		}
		switch (state.waveType)
		{
		case 1:  state.waveTypeName = "Heat"; break;
		case 2:  state.waveTypeName = "Cold"; break;
		default: state.waveTypeName = "None"; break;
		}

		// Timers are taken straight from the packet — no elapsed-time interpolation
		// here because the packet was just received (age ≈ 0 ms).  The HUD overlay's
		// own QPC-based interpolation handles sub-second smoothing from this point on.
		state.phaseRemainingSeconds = pkt.phaseRemainingSeconds;
		state.nextRuptureInSeconds = pkt.nextRuptureInSeconds;
		state.stableRemaining = pkt.stableRemaining;

		// Per-phase breakdown fields are not carried in the sync packet.
		state.warningRemaining = -1.0f;
		state.burningRemaining = -1.0f;
		state.coolingRemaining = -1.0f;
		state.stabilizingRemaining = -1.0f;

		// Diagnostic fields
		state.diag.codePath = "netSync";
		state.diag.rawStage = static_cast<int>(pkt.rawStage);
		state.diag.rawWaveType = static_cast<int>(pkt.waveType);
		state.diag.rawPaused = (pkt.paused != 0);
		state.diag.rawNextPhase = static_cast<int32_t>(pkt.waveNumber);
		switch (pkt.rawStage)
		{
		case 0:  state.diag.rawPhaseName = "None";     break;
		case 1:  state.diag.rawPhaseName = "PreWave";  break;
		case 2:  state.diag.rawPhaseName = "Moving";   break;
		case 3:  state.diag.rawPhaseName = "Fadeout";  break;
		case 4:  state.diag.rawPhaseName = "Growback"; break;
		default: state.diag.rawPhaseName = "Stage?";   break;
		}

		return state;
	}

	// ---------------------------------------------------------------------------
	// World-scoped object cache.
	// Populated once by OnWorldReady() when the experience finishes loading,
	// nulled by OnWorldTeardown() on world exit.  ReadCurrentState() reads these
	// directly — zero scanning, zero world-pointer comparisons per tick.
	// ---------------------------------------------------------------------------
	static SDK::UCrEnviroWaveSubsystem* s_waveSub = nullptr;
	static SDK::ACrGatherableSpawnersRepActor* s_repActor = nullptr;

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
			// Guard GetName() — Class is already checked non-null above
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

		for (int i = 0; i < arr->Num(); i++)
		{
			SDK::UObject* obj = arr->GetByIndex(i);
			if (!obj || !obj->Class) continue;
			// Outer chain: actor → ULevel → UWorld — both levels must be non-null
			if (!obj->Outer || !obj->Outer->Outer) continue;
			if (obj->Outer->Outer != static_cast<SDK::UObject*>(world)) continue;
			if (obj->Class->GetName() == "CrGatherableSpawnersRepActor")
				return static_cast<SDK::ACrGatherableSpawnersRepActor*>(obj);
		}
		return nullptr;
	}

	// ---------------------------------------------------------------------------
	// Called once from the plugin's OnExperienceLoadComplete after the gameplay
	// world is fully stood up.  Runs the GObjects scans here so ReadCurrentState()
	// never has to scan on the hot per-tick path.
	// ---------------------------------------------------------------------------
	void OnWorldReady()
	{
		if (SDK::UWorld* world = SDK::UWorld::GetWorld())
		{
			s_waveSub = FindEnviroWaveSubsystem(world);
			s_repActor = FindGatherableSpawnersRepActor(world);
			LOG_INFO("OnWorldReady: waveSub=%s repActor=%s",
				s_waveSub ? "FOUND" : "absent",
				s_repActor ? "FOUND" : "absent");
		}
	}

	// ---------------------------------------------------------------------------
	// Called from OnBeforeWorldEndPlay / the non-gameplay world branch so that
	// ReadCurrentState() never touches dangling pointers during teardown.
	// ---------------------------------------------------------------------------
	void OnWorldTeardown()
	{
		LOG_DEBUG("OnWorldTeardown: clearing cached world objects");
		s_waveSub = nullptr;
		s_repActor = nullptr;
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
	//   Stable → Burning:    nextTimeRemaining jumps from ~0 to ~BURNING_DURATION
	//   Burning → Cooling:   nextTimeRemaining jumps from ~0 to a large value (full cycle ahead)
	//   Cooling → Stabilizing:  elapsed time since Cooling start >= COOLING_DURATION
	//   Stabilizing → Stable:   NextPhase increments (server broadcasts new wave number)
	// ---------------------------------------------------------------------------
	static constexpr float BURNING_DURATION = 30.0f;
	static constexpr float COOLING_DURATION = 60.0f;
	static constexpr float STABILIZING_DURATION = 600.0f;

	struct ClientPhaseTracker
	{
		RupturePhase phase = RupturePhase::Unknown;
		int32_t prevNextPhase = -1;
		SDK::UWorld* lastWorld = nullptr;
		bool    initialized = false;
		float        lastObservedStableDuration = 2550.0f; // default canonical Stable duration
		float        phase3StartRemaining = -1.0f;   // nextTimeRemaining when nextPhase first hit 3
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
			s_tracker.initialized = true;
		}

		switch (nextPhase)
		{
		case 0:s_tracker.phase = RupturePhase::Stable;  break;
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
			state.phase = RupturePhase::Stable;
			state.phaseName = "Stable";
			state.phaseRemainingSeconds = nextTimeRemaining;
			state.nextRuptureInSeconds = nextTimeRemaining;
			state.stableRemaining = nextTimeRemaining;
			break;

		case RupturePhase::Warning:
			state.phase = RupturePhase::Warning;
			state.phaseName = "Warning";
			state.phaseRemainingSeconds = nextTimeRemaining;
			state.nextRuptureInSeconds = nextTimeRemaining;
			break;

		case RupturePhase::Burning:
			state.phase = RupturePhase::Burning;
			state.phaseName = "Burning";
			state.phaseRemainingSeconds = nextTimeRemaining;
			state.nextRuptureInSeconds = 0.0f;
			state.burningRemaining = nextTimeRemaining;
			break;

		case RupturePhase::Cooling:
		{
			float coolingBoundary = (s_tracker.phase3StartRemaining > 0.0f)
				? (s_tracker.phase3StartRemaining - COOLING_DURATION) : STABILIZING_DURATION;
			float coolingRemaining = nextTimeRemaining - coolingBoundary;
			if (coolingRemaining < 0.0f) coolingRemaining = 0.0f;
			state.phase = RupturePhase::Cooling;
			state.phaseName = "Cooling";
			state.phaseRemainingSeconds = coolingRemaining;
			state.nextRuptureInSeconds = nextTimeRemaining + s_tracker.lastObservedStableDuration;
			state.coolingRemaining = coolingRemaining;
			state.stabilizingRemaining = coolingBoundary;
			state.stableRemaining = s_tracker.lastObservedStableDuration;
			break;
		}

		case RupturePhase::Stabilizing:
			state.phase = RupturePhase::Stabilizing;
			state.phaseName = "Stabilizing";
			state.phaseRemainingSeconds = nextTimeRemaining;
			state.nextRuptureInSeconds = nextTimeRemaining + s_tracker.lastObservedStableDuration;
			state.stabilizingRemaining = nextTimeRemaining;
			state.stableRemaining = s_tracker.lastObservedStableDuration;
			break;

		default:
			state.phase = RupturePhase::Unknown;
			state.phaseName = "Unknown";
			break;
		}
	}

	// ---------------------------------------------------------------------------
	// Compute the total duration of each stage from the game's own settings.
	// These are the values the server/client agreed on — not hardcoded constants.
	//
	// FCrEnviroWaveSettings fields used per stage:
	//   PreWave:  PreWaveDuration + WavePreWaveExplosionDuration
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
	TimerState ReadCurrentState()
	{
		LOG_TRACE("ReadCurrentState: enter");

		TimerState state{};
		state.valid = false;
		state.phase = RupturePhase::Unknown;
		state.phaseName = "Unknown";
		state.phaseRemainingSeconds = -1.0f;
		state.nextRuptureInSeconds = -1.0f;
		state.waveNumber = 0;
		state.paused = false;
		state.waveType = 0;
		state.waveTypeName = "None";
		state.warningRemaining = -1.0f;
		state.burningRemaining = -1.0f;
		state.coolingRemaining = -1.0f;
		state.stabilizingRemaining = -1.0f;
		state.stableRemaining = -1.0f;

		// Initialize diagnostic raw data
		state.diag.codePath = "none";
		state.diag.rawStage = -1;
		state.diag.rawWaveType = -1;
		state.diag.rawNextTime = 0.0f;
		state.diag.rawServerTime = 0.0;
		state.diag.rawNextTimeRemaining = 0.0f;
		state.diag.rawNextPhase = -1;
		state.diag.rawPaused = false;
		state.diag.hasRepActor = false;
		state.diag.hasSubsystem = false;
		state.diag.rawProgress = -1.0f;
		memset(state.diag.repActorBytes, 0, sizeof(state.diag.repActorBytes));
		state.diag.repActorBytesValid = false;
		state.diag.rawPhaseName = "?";

		SDK::UWorld* world = SDK::UWorld::GetWorld();
		LOG_TRACE("ReadCurrentState: UWorld::GetWorld()=%p", static_cast<void*>(world));
		if (!world) { LOG_WARN_ONCE("ReadCurrentState: UWorld is null"); return state; }

		// Get game state for the replicated WaveTimerActor
		auto* gameState = static_cast<SDK::ACrGameStateBase*>(world->GameState);
		LOG_TRACE("ReadCurrentState: world->GameState=%p", static_cast<void*>(gameState));
		if (!gameState) { LOG_WARN_ONCE("ReadCurrentState: GameState is null"); return state; }

		LOG_TRACE("ReadCurrentState: gameState->WaveTimerActor=%p", static_cast<void*>(gameState->WaveTimerActor));
		if (!gameState->WaveTimerActor) { LOG_WARN_ONCE("ReadCurrentState: WaveTimerActor is null (not yet replicated?)"); return state; }

		SDK::ACrWaveTimerActor* timerActor = gameState->WaveTimerActor;
		LOG_TRACE("ReadCurrentState: timerActor=%p", static_cast<void*>(timerActor));

		state.valid = true;
		state.waveNumber = timerActor->NextPhase;
		state.paused = timerActor->bPause;
		LOG_TRACE("ReadCurrentState: waveNumber=%d paused=%d", state.waveNumber, static_cast<int>(state.paused));

		double serverTime = gameState->GetServerWorldTimeSeconds();
		float rawUnclamped = timerActor->NextTime - static_cast<float>(serverTime);
		float nextTimeRemaining = (rawUnclamped < 0.0f) ? 0.0f : rawUnclamped;
		LOG_TRACE("ReadCurrentState: serverTime=%.2f NextTime=%.2f rawUnclamped=%.2f nextTimeRemaining=%.2f",
			serverTime, static_cast<double>(timerActor->NextTime), static_cast<double>(rawUnclamped), static_cast<double>(nextTimeRemaining));

		state.diag.rawNextTime = timerActor->NextTime;
		state.diag.rawServerTime = serverTime;
		state.diag.rawNextTimeRemaining = rawUnclamped;
		state.diag.rawNextPhase = timerActor->NextPhase;
		state.diag.rawPaused = timerActor->bPause;

		// Use the pre-cached pointers populated by OnWorldReady() — no GObjects scan.
		SDK::UCrEnviroWaveSubsystem* waveSub = s_waveSub;
		state.diag.hasSubsystem = (waveSub != nullptr);
		LOG_TRACE("ReadCurrentState: waveSub=%p nextTimeRemaining=%.1f waveNumber=%d",
			static_cast<void*>(waveSub), nextTimeRemaining, timerActor->NextPhase);

		// ------------------------------------------------------------------
		// Network sync path
		// ------------------------------------------------------------------
		LOG_TRACE("ReadCurrentState: s_netSync.valid=%d s_everReceivedServerPacket=%d",
			static_cast<int>(s_netSync.valid), static_cast<int>(s_everReceivedServerPacket));
		if (!waveSub && s_netSync.valid)
		{
			ULONGLONG ageMs = GetTickCount64() - s_netSync.receivedAtMs;
			bool fresh = ageMs < static_cast<ULONGLONG>(NET_SYNC_STALE_SEC * 1000.0f);
			LOG_TRACE("ReadCurrentState: netSync ageMs=%llu fresh=%d everReceived=%d",
				static_cast<unsigned long long>(ageMs), static_cast<int>(fresh), static_cast<int>(s_everReceivedServerPacket));

			if (fresh || s_everReceivedServerPacket)
			{
				const TimerSyncPacket& p = s_netSync.pkt;

				state.diag.rawStage = static_cast<int>(p.rawStage);
				state.phase = static_cast<RupturePhase>(p.phase);
				state.waveType = p.waveType;
				state.paused = (p.paused != 0);
				state.waveNumber = p.waveNumber;

				switch (p.rawStage)
				{
				case 0:  state.diag.rawPhaseName = "None";     break;
				case 1:  state.diag.rawPhaseName = "PreWave";  break;
				case 2:  state.diag.rawPhaseName = "Moving";   break;
				case 3:  state.diag.rawPhaseName = "Fadeout";  break;
				case 4:  state.diag.rawPhaseName = "Growback"; break;
				default: state.diag.rawPhaseName = "Stage?";   break;
				}
				switch (state.phase)
				{
				case RupturePhase::Stable:state.phaseName = "Stable";      break;
				case RupturePhase::Warning:     state.phaseName = "Warning";     break;
				case RupturePhase::Burning:     state.phaseName = "Burning";     break;
				case RupturePhase::Cooling:  state.phaseName = "Cooling";  break;
				case RupturePhase::Stabilizing: state.phaseName = "Stabilizing"; break;
				default:       state.phaseName = "Unknown";     break;
				}
				switch (state.waveType)
				{
				case 1:  state.waveTypeName = "Heat"; break;
				case 2:  state.waveTypeName = "Cold"; break;
				default: state.waveTypeName = "None"; break;
				}

				if (fresh)
				{
					float elapsed = static_cast<float>(ageMs) / 1000.0f;
					auto Interp = [elapsed](float base) -> float {
						return (base >= 0.0f) ? fmaxf(0.0f, base - elapsed) : -1.0f;
						};
					state.phaseRemainingSeconds = Interp(p.phaseRemainingSeconds);
					state.nextRuptureInSeconds = Interp(p.nextRuptureInSeconds);
					state.stableRemaining = Interp(p.stableRemaining);
					state.diag.codePath = "netSync";
				}
				else
				{
					state.phaseRemainingSeconds = p.phaseRemainingSeconds;
					state.nextRuptureInSeconds = p.nextRuptureInSeconds;
					state.stableRemaining = p.stableRemaining;
					state.diag.codePath = "netSync-stale";
					LOG_DEBUG("ReadCurrentState: netSync stale (%.0fs) — showing frozen server data",
						static_cast<float>(ageMs) / 1000.0f);
				}

				LOG_TRACE("ReadCurrentState: returning netSync state phase=%d", static_cast<int>(state.phase));
				return state;
			}

			LOG_TRACE("ReadCurrentState: netSync stale and never paired — falling back to local");
			LOG_DEBUG("ReadCurrentState: netSync packet is stale (%.0fs) and no prior pairing — falling back",
				static_cast<float>(ageMs) / 1000.0f);
		}

		bool nextTimeValid = (rawUnclamped >= -60.0f);
		LOG_TRACE("ReadCurrentState: nextTimeValid=%d rawUnclamped=%.1f", static_cast<int>(nextTimeValid), rawUnclamped);
		if (!nextTimeValid)
			LOG_WARN_ONCE("ReadCurrentState: NextTime is %.0fs in the past — timing unavailable, phase detection only (this can happen at startup)", rawUnclamped);

		if (!waveSub)
		{
			LOG_TRACE("ReadCurrentState: no waveSub — trying repActor path");
			LOG_DEBUG("ReadCurrentState: UCrEnviroWaveSubsystem absent — using replication-actor mode");

			SDK::ACrGatherableSpawnersRepActor* repActor = s_repActor;
			state.diag.hasRepActor = (repActor != nullptr);
			LOG_TRACE("ReadCurrentState: repActor=%p", static_cast<void*>(repActor));
			if (repActor)
			{
				state.diag.codePath = "repActor";

				const uint8_t* base = reinterpret_cast<const uint8_t*>(repActor);
				memcpy(state.diag.repActorBytes, base + 0x02A8, 8);
				state.diag.repActorBytesValid = true;

				SDK::EEnviroWaveStage repStage = repActor->RepEnviroWaveStageChange;
				SDK::EEnviroWave      repWave = repActor->RepEnviroWaveTypeChange;
				LOG_TRACE("ReadCurrentState: repStage=%d repWave=%d", static_cast<int>(repStage), static_cast<int>(repWave));

				state.diag.rawStage = static_cast<int>(repStage);
				state.diag.rawWaveType = static_cast<int>(repWave);
				switch (repStage)
				{
				case SDK::EEnviroWaveStage::None:     state.diag.rawPhaseName = "None";     break;
				case SDK::EEnviroWaveStage::PreWave:  state.diag.rawPhaseName = "PreWave";  break;
				case SDK::EEnviroWaveStage::Moving:   state.diag.rawPhaseName = "Moving";   break;
				case SDK::EEnviroWaveStage::Fadeout:  state.diag.rawPhaseName = "Fadeout";break;
				case SDK::EEnviroWaveStage::Growback: state.diag.rawPhaseName = "Growback"; break;
				default:       state.diag.rawPhaseName = "Stage?";   break;
				}

				LOG_DEBUG("ReadCurrentState: repActor FOUND — repStage=%d repWave=%d nextTimeValid=%s",
					static_cast<int>(repStage), static_cast<int>(repWave), nextTimeValid ? "Y" : "N");

				state.waveType = static_cast<uint8_t>(repWave);
				switch (repWave)
				{
				case SDK::EEnviroWave::Heat: state.waveTypeName = "Heat"; break;
				case SDK::EEnviroWave::Cold: state.waveTypeName = "Cold"; break;
				default: state.waveTypeName = "None"; break;
				}

				switch (repStage)
				{
				case SDK::EEnviroWaveStage::None:
					state.phase = RupturePhase::Stable;
					state.phaseName = "Stable";
					if (nextTimeValid)
					{
						state.phaseRemainingSeconds = nextTimeRemaining;
						state.nextRuptureInSeconds = nextTimeRemaining;
						state.stableRemaining = nextTimeRemaining;
					}
					break;
				case SDK::EEnviroWaveStage::PreWave:
					state.phase = RupturePhase::Warning;
					state.phaseName = "Warning";
					if (nextTimeValid)
					{
						state.phaseRemainingSeconds = nextTimeRemaining;
						state.nextRuptureInSeconds = nextTimeRemaining;
					}
					break;
				case SDK::EEnviroWaveStage::Moving:
					state.phase = RupturePhase::Burning;
					state.phaseName = "Burning";
					state.nextRuptureInSeconds = 0.0f;
					break;
				case SDK::EEnviroWaveStage::Fadeout:
					state.phase = RupturePhase::Cooling;
					state.phaseName = "Cooling";
					if (nextTimeValid)
					{
						state.nextRuptureInSeconds = nextTimeRemaining;
						state.stableRemaining = nextTimeRemaining;
					}
					break;
				case SDK::EEnviroWaveStage::Growback:
					state.phase = RupturePhase::Stabilizing;
					state.phaseName = "Stabilizing";
					if (nextTimeValid)
					{
						state.nextRuptureInSeconds = nextTimeRemaining;
						state.stableRemaining = nextTimeRemaining;
					}
					break;
				default:
					state.phase = RupturePhase::Unknown;
					state.phaseName = "Unknown";
					break;
				}

				LOG_TRACE("ReadCurrentState: returning repActor state phase=%d", static_cast<int>(state.phase));
			}
			else if (nextTimeValid)
			{
				LOG_TRACE("ReadCurrentState: no repActor — using stateMachine nextPhase=%d nextTimeRemaining=%.1f",
					timerActor->NextPhase, nextTimeRemaining);
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
					static_cast<float>(serverTime), nextTimeRemaining,
					timerActor->NextPhase, world);

				FillStateFromStateMachine(state, nextTimeRemaining);
				LOG_TRACE("ReadCurrentState: stateMachine result phase=%d", static_cast<int>(state.phase));
			}
			else
			{
				LOG_TRACE("ReadCurrentState: no subsystem/repActor and NextTime stale — returning Waiting");
				state.diag.codePath = "none";
				state.phase = RupturePhase::Unknown;
				state.phaseName = "Waiting";
			}
			return state;
		}

		// --- Full mode: subsystem present ---
		LOG_TRACE("ReadCurrentState: subsystem path — waveSub=%p", static_cast<void*>(waveSub));
		state.diag.codePath = "subsystem";

		// The subsystem is the authoritative source for pause state on the server.
		// timerActor->bPause is a replicated field — unreliable server-side.
		state.paused = waveSub->IsWavePaused();

		SDK::EEnviroWave waveType = waveSub->GetCurrentType();
		LOG_TRACE("ReadCurrentState: GetCurrentType()=%d", static_cast<int>(waveType));
		state.waveType = static_cast<uint8_t>(waveType);
		switch (waveType)
		{
		case SDK::EEnviroWave::Heat: state.waveTypeName = "Heat"; break;
		case SDK::EEnviroWave::Cold: state.waveTypeName = "Cold"; break;
		default:    state.waveTypeName = "None"; break;
		}

		SDK::EEnviroWaveStage stage = waveSub->GetCurrentStage();
		LOG_TRACE("ReadCurrentState: GetCurrentStage()=%d", static_cast<int>(stage));
		state.diag.rawStage = static_cast<int>(stage);
		state.diag.rawWaveType = static_cast<int>(waveType);
		switch (stage)
		{
		case SDK::EEnviroWaveStage::None:     state.diag.rawPhaseName = "None";     break;
		case SDK::EEnviroWaveStage::PreWave:  state.diag.rawPhaseName = "PreWave";  break;
		case SDK::EEnviroWaveStage::Moving:   state.diag.rawPhaseName = "Moving";   break;
		case SDK::EEnviroWaveStage::Fadeout:  state.diag.rawPhaseName = "Fadeout";  break;
		case SDK::EEnviroWaveStage::Growback: state.diag.rawPhaseName = "Growback"; break;
		default:              state.diag.rawPhaseName = "Stage?";   break;
		}
		LOG_DEBUG("ReadCurrentState: subsystem stage=%d waveType=%d nextTimeRemaining=%.1f",
			static_cast<int>(stage), static_cast<int>(waveType), nextTimeRemaining);

		if (stage == SDK::EEnviroWaveStage::None)
		{
			LOG_TRACE("ReadCurrentState: stage=None → Stable, returning");
			state.phase = RupturePhase::Stable;
			state.phaseName = "Stable";

			// NextTime is a replicated field populated for clients only.
			// On a server that has been running for a long time, NextTime retains
			// its startup value (e.g. 2400 s) while GetServerWorldTimeSeconds()
			// has advanced to hundreds of thousands of seconds, making
			// rawUnclamped deeply negative and nextTimeRemaining = 0.
			//
			// Fallback: derive stable remaining from GetTimeSinceLastWaveStarted().
			// Full cycle duration = Stable + Warning + Burning + Cooling + Stabilizing.
			// We use the empirically-validated canonical duration when settings are
			// unavailable; when settings are available we compute it properly.
			if (nextTimeValid)
			{
				// NextTime is live (client or a freshly started server) — use it directly.
				state.phaseRemainingSeconds = nextTimeRemaining;
				state.nextRuptureInSeconds = nextTimeRemaining;
				state.stableRemaining = nextTimeRemaining;
			}
			else
			{
				// NextTime is stale — reconstruct from how long ago the last wave ended.
				// GetTimeSinceLastWaveStarted() returns seconds since the last wave began
				// (i.e. since Burning/Moving started), so during Stable it equals:
				// burningDuration + coolingDuration + stabilizingDuration + stableElapsed
				// Therefore: stableElapsed = timeSince - burning - cooling - stabilizing
				//   stableRemaining = fullStable - stableElapsed
				//
				// Do NOT call GetCurrentStageSettings() here — it dereferences a wave
				// settings pointer that is null when no wave is in progress (stage=None),
				// which would crash the server. Use the empirical fallback durations instead.
				float fullBurning = BURNING_DURATION;
				float fullCooling = COOLING_DURATION;
				float fullStabilizing = STABILIZING_DURATION;
				float fullStable = s_tracker.lastObservedStableDuration;

				float timeSinceWaveStart = static_cast<float>(waveSub->GetTimeSinceLastWaveStarted());
				float postWaveDuration = fullBurning + fullCooling + fullStabilizing;
				float stableElapsed = timeSinceWaveStart - postWaveDuration;
				if (stableElapsed < 0.0f) stableElapsed = 0.0f;
				float stableRem = fullStable - stableElapsed;
				if (stableRem < 0.0f) stableRem = 0.0f;

				LOG_DEBUG("ReadCurrentState: NextTime stale — timeSinceWaveStart=%.1f postWave=%.1f stableElapsed=%.1f stableRem=%.1f",
					timeSinceWaveStart, postWaveDuration, stableElapsed, stableRem);

				state.phaseRemainingSeconds = stableRem;
				state.nextRuptureInSeconds = stableRem;
				state.stableRemaining = stableRem;
			}
			return state;
		}

		LOG_TRACE("ReadCurrentState: calling GetCurrentStageSettings()");
		SDK::FCrEnviroWaveSettings settings = waveSub->GetCurrentStageSettings();

		LOG_TRACE("ReadCurrentState: calling GetCurrentStageProgress()");
		float progress = waveSub->GetCurrentStageProgress();
		LOG_TRACE("ReadCurrentState: progress=%.4f", progress);
		state.diag.rawProgress = progress;
		if (progress < 0.0f) progress = 0.0f;
		if (progress > 1.0f) progress = 1.0f;

		float stageDuration = StageDurationFromSettings(stage, settings);
		float phaseRemaining = stageDuration * (1.0f - progress);
		if (phaseRemaining < 0.0f) phaseRemaining = 0.0f;
		LOG_TRACE("ReadCurrentState: stageDuration=%.1f phaseRemaining=%.1f", stageDuration, phaseRemaining);

		float stableCountdown = nextTimeRemaining;
		float fullBurning = StageDurationFromSettings(SDK::EEnviroWaveStage::Moving, settings);
		float fullCooling = StageDurationFromSettings(SDK::EEnviroWaveStage::Fadeout, settings);
		float fullStabilizing = StageDurationFromSettings(SDK::EEnviroWaveStage::Growback, settings);
		LOG_TRACE("ReadCurrentState: fullBurning=%.1f fullCooling=%.1f fullStabilizing=%.1f",
			fullBurning, fullCooling, fullStabilizing);

		switch (stage)
		{
		case SDK::EEnviroWaveStage::PreWave:
			state.phase = RupturePhase::Warning;
			state.phaseName = "Warning";
			state.phaseRemainingSeconds = phaseRemaining;
			state.nextRuptureInSeconds = phaseRemaining;
			state.warningRemaining = phaseRemaining;
			state.burningRemaining = fullBurning;
			state.coolingRemaining = fullCooling;
			state.stabilizingRemaining = fullStabilizing;
			break;
		case SDK::EEnviroWaveStage::Moving:
			state.phase = RupturePhase::Burning;
			state.phaseName = "Burning";
			state.phaseRemainingSeconds = phaseRemaining;
			state.nextRuptureInSeconds = 0.0f;
			state.warningRemaining = 0.0f;
			state.burningRemaining = phaseRemaining;
			state.coolingRemaining = fullCooling;
			state.stabilizingRemaining = fullStabilizing;
			break;
		case SDK::EEnviroWaveStage::Fadeout:
			state.phase = RupturePhase::Cooling;
			state.phaseName = "Cooling";
			state.phaseRemainingSeconds = phaseRemaining;
			state.nextRuptureInSeconds = phaseRemaining + fullStabilizing + stableCountdown;
			state.warningRemaining = 0.0f;
			state.burningRemaining = 0.0f;
			state.coolingRemaining = phaseRemaining;
			state.stabilizingRemaining = fullStabilizing;
			state.stableRemaining = stableCountdown;
			break;
		case SDK::EEnviroWaveStage::Growback:
			state.phase = RupturePhase::Stabilizing;
			state.phaseName = "Stabilizing";
			state.phaseRemainingSeconds = phaseRemaining;
			state.nextRuptureInSeconds = phaseRemaining + stableCountdown;
			state.warningRemaining = 0.0f;
			state.burningRemaining = 0.0f;
			state.coolingRemaining = 0.0f;
			state.stabilizingRemaining = phaseRemaining;
			state.stableRemaining = stableCountdown;
			break;
		default:
			state.phase = RupturePhase::Unknown;
			state.phaseName = "Unknown";
			break;
		}

		LOG_TRACE("ReadCurrentState: returning subsystem state phase=%d rem=%.1f",
			static_cast<int>(state.phase), state.phaseRemainingSeconds);
		return state;
	}

	// ---------------------------------------------------------------------------
	// Server-side broadcast.
	// Builds a TimerSyncPacket from state and sends it to all clients.
	// ---------------------------------------------------------------------------
	void BroadcastState(const TimerState& state, IPluginHooks* hooks, const IPluginSelf* self)
	{
		if (!state.valid) return;
		if (!hooks || !hooks->Network) return;

		TimerSyncPacket pkt{};
		pkt.phaseRemainingSeconds = state.phaseRemainingSeconds;
		pkt.nextRuptureInSeconds = state.nextRuptureInSeconds;
		pkt.stableRemaining = state.stableRemaining;
		pkt.waveNumber = state.waveNumber;
		pkt.phase = static_cast<uint8_t>(state.phase);
		pkt.waveType = state.waveType;
		pkt.paused = state.paused ? 1 : 0;
		pkt.rawStage = (state.diag.rawStage >= 0)
			? static_cast<uint8_t>(state.diag.rawStage) : 0;

		Network::SendPacketToAllClients(hooks, self, pkt);
		LOG_DEBUG("BroadcastState: phase=%d rem=%.1f nextRup=%.1f",
			static_cast<int>(pkt.phase), pkt.phaseRemainingSeconds, pkt.nextRuptureInSeconds);
	}

	// ---------------------------------------------------------------------------
	// Client-side receive registration.
	// Wraps OnReceive<TimerSyncPacket> and converts the packet to a TimerState
	// via ApplyNetworkSync() before forwarding to the caller's callback.
	// ---------------------------------------------------------------------------
	void RegisterClientReceive(IPluginHooks* hooks, const IPluginSelf* self,
		std::function<void(const TimerState&)> callback)
	{
		if (!hooks || !hooks->Network || !callback) return;

		Network::OnReceive<TimerSyncPacket>(hooks, self,
			[cb = std::move(callback)](const TimerSyncPacket& pkt)
			{
				TimerState state = ApplyNetworkSync(pkt);
				cb(state);
			});

		LOG_DEBUG("RegisterClientReceive: registered TimerSyncPacket handler");
	}

} // namespace RuptureTimer
