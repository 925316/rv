#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdio>

#include "unreal.hpp"

namespace rv
{
	namespace helper
	{
		// cm/s, cm/s^2.
		constexpr double MAX_SPEED = 10000.0;  // runaway clamp (100 m/s)
		constexpr double LIFT_ACCEL = 1200.0;   // climb, per second
		constexpr double FORWARD_ACCEL = 1500.0;   // W, per second while held
		constexpr double BRAKE_ACCEL = 2500.0;   // S
		constexpr double STRAFE_ACCEL = 1200.0;   // A/D, per second

		// rad/s^2 (bAccelChange = true, so inertia does not enter).
		constexpr double MAX_ACCEL = 20.0;     // total angular accel clamp
		constexpr double LEVEL_GAIN = 6.0;      // per rad of pitch / roll error
		constexpr double YAW_GAIN = 4.0;      // per rad of yaw error
		constexpr double DAMP = 2.5;      // per rad/s of angular velocity

		struct state
		{
			unreal::FRotator m_takeoff_attitude{};
			unreal::FVector m_kin_vel{};
			unreal::AVehicleBase* m_vehicle{ nullptr };
			unreal::UPrimitiveComponent* m_mesh{ nullptr };
			double m_saved_spring_downforce{ 0.0 };    // AVS snapshot at Engage,
			double m_saved_base_linear_drag{ 0.0 };     // restored on Disengage
			double m_saved_default_linear_drag{ 0.0 };
			double m_saved_linear_damping{ 0.0 };
			ULONGLONG m_last_step_ms{ 0 };
			bool m_saved_dynamic_air_drag{ false };
			bool m_engaged{ false };
			bool m_saved_gravity{ true };
			bool m_saved_block_down_force{ false };
		};

		inline state g_state;

		// g_world: level-transition detector - live world is re-fetched every tick.
		inline unreal::UWorld* g_world = nullptr;
		inline HWND         g_game_wnd = nullptr;
		inline WNDPROC      g_orig_wnd_proc = nullptr;

		constexpr UINT_PTR TICK_TIMER_ID = 0x52494445; // arbitrary, non-zero
		constexpr UINT     TICK_PERIOD_MS = 8;          // 8 ms cadence

		// Set by the DETACH path; the main_thread watchdog loop polls it.
		inline std::atomic<bool> g_shutdown{ false };
		// Runs once: both main_thread exit and DllMain DETACH funnel through
		// shutdown(), whichever arrives first wins.
		inline std::atomic<bool> g_cleanup_done{ false };

		inline HMODULE g_module = nullptr;
		inline HANDLE g_main_thread = nullptr;
		inline FILE* g_console_out = nullptr;
	}
}
