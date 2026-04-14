#pragma once

#include "timer_tracker.h"

struct IPluginHooks;

namespace HudOverlay
{
	// Register the PostRender callback via hooks->HUD (v16, client only).
	// Returns false if hooks->HUD is null (server build) or registration fails.
	bool Install(IPluginHooks* hooks);

	// Unregister the PostRender callback. Safe to call if Install was never called or failed.
	void Remove(IPluginHooks* hooks);

	// Push a fresh timer state for the overlay to display.
	// Call from the engine tick callback after ReadCurrentState().
	void SetState(const RuptureTimer::TimerState& state);

	// Reset all display state on world teardown (BeforeWorldEndPlay).
	// Clears the last known timer state and all smoothed interpolation values
	// so stale data never bleeds into the next world load.
	void Reset();
}
