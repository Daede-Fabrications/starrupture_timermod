#pragma once

#include "plugin_interface.h"

namespace RuptureTimerConfig
{
	static const ConfigEntry CONFIG_ENTRIES[] = {
		// --- General ---
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable the RuptureTimer plugin",
			0.0f, 0.0f
		},
		// --- Export (StreamDeck / external tools) ---
		{
			"Export",
			"WriteJsonFile",
			ConfigValueType::Boolean,
			"false", // Dont force un-needed IO on clients that most of the time wont be needing the JSON output
			"Write rupture timer state to a JSON file for external tools (e.g. StreamDeck)",
			0.0f, 0.0f
		},
		{
			"Export",
			"JsonFilePath",
			ConfigValueType::String,
			"Plugins/data/rupture_timer.json",
			"Path to the JSON output file (relative to game directory)",
			0.0f, 0.0f
		},
		{
			"Export",
			"UpdateIntervalSeconds",
			ConfigValueType::Float,
			"1.0",
			"How often (in seconds) to update the JSON file",
			0.1f, 30.0f
		},
		{
			"Export",
			"ExtendedPhaseTimers",
			ConfigValueType::Boolean,
			"false",
			"Add per-phase timing breakdown to JSON: warning_remaining_sec, burning_remaining_sec, cooling_remaining_sec, stabilizing_remaining_sec, stable_remaining_sec",
			0.0f, 0.0f
		},
		{
			"Export",
			"WriteDiagnosticLog",
			ConfigValueType::Boolean,
			"false",
			"Append raw game values to a diagnostic log file for debugging phase/timer issues over a full game session",
			0.0f, 0.0f
		},
		{
			"Export",
			"DiagnosticLogPath",
			ConfigValueType::String,
			"Plugins/data/timer_diagnostic.log",
			"Path to the diagnostic log file (relative to game directory)",
			0.0f, 0.0f
		},
		// --- HUD (in-game overlay) ---
		{
			"HUD",
			"ShowOverlay",
			ConfigValueType::Boolean,
			"true", // Most people will want this on.
			"Show rupture timer as an in-game text overlay",
			0.0f, 0.0f
		},
		{
			"HUD",
			"Position",
			ConfigValueType::String,
			"LowerLeft",
			"Overlay anchor position: LowerLeft, MidLeft, TopLeft, TopMid, TopRight, MidRight, LowerRight",
			0.0f, 0.0f
		},
		{
			"HUD",
			"Scale",
			ConfigValueType::Float,
			"1.0",
			"Text scale multiplier (1.0 = default engine font size)",
			0.25f, 5.0f
		},
		{
			"HUD",
			"ShowDebugInfo",
			ConfigValueType::Boolean,
			"false",
			"Append raw diagnostic lines to the overlay: stage int, wave number, raw timer values. Use to diagnose phase-reading issues.",
			0.0f, 0.0f
		},
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	class Config
	{
	public:
		static void Initialize(IPluginSelf* self)
		{
			s_self = self;
			if (s_self && s_self->config)
				s_self->config->InitializeFromSchema(s_self, &SCHEMA);
		}

		// --- General ---
		static bool IsEnabled()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "General", "Enabled", true) : true;
		}

		// --- Export ---
		static bool ShouldWriteJsonFile()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "Export", "WriteJsonFile", true) : true;
		}

		static const char* GetJsonFilePath()
		{
			static char buffer[512];
			if (s_self && s_self->config && s_self->config->ReadString(s_self, "Export", "JsonFilePath", buffer, sizeof(buffer), "Plugins/data/rupture_timer.json"))
				return buffer;
			return "Plugins/data/rupture_timer.json";
		}

		static float GetUpdateIntervalSeconds()
		{
			return (s_self && s_self->config) ? s_self->config->ReadFloat(s_self, "Export", "UpdateIntervalSeconds", 1.0f) : 1.0f;
		}

		static bool ShouldWriteExtendedPhaseTimers()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "Export", "ExtendedPhaseTimers", false) : false;
		}

		static bool ShouldWriteDiagnosticLog()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "Export", "WriteDiagnosticLog", false) : false;
		}

		static const char* GetDiagnosticLogPath()
		{
			static char buffer[512];
			if (s_self && s_self->config && s_self->config->ReadString(s_self, "Export", "DiagnosticLogPath", buffer, sizeof(buffer), "Plugins/data/timer_diagnostic.log"))
				return buffer;
			return "Plugins/data/timer_diagnostic.log";
		}

		// --- HUD ---
		static bool ShouldShowOverlay()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "HUD", "ShowOverlay", false) : false;
		}

		static const char* GetOverlayPosition()
		{
			static char buffer[64];
			if (s_self && s_self->config && s_self->config->ReadString(s_self, "HUD", "Position", buffer, sizeof(buffer), "LowerLeft"))
				return buffer;
			return "LowerLeft";
		}

		static float GetOverlayScale()
		{
			float scale = (s_self && s_self->config) ? s_self->config->ReadFloat(s_self, "HUD", "Scale", 1.0f) : 1.0f;
			if (scale < 0.25f) scale = 0.25f;
			if (scale > 5.0f)  scale = 5.0f;
			return scale;
		}

		static bool ShouldShowDebugInfo()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "HUD", "ShowDebugInfo", false) : false;
		}

	private:
		static IPluginSelf* s_self;
	};
}
