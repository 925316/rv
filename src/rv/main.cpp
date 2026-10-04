#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "resolve.hpp"
#include "signatures.hpp"
#include "unreal.hpp"

// user32 is not in cl.exe's default libs.
#pragma comment(lib, "user32.lib")

/*
 * SHIFT           kinematic flight: own velocity, overwrites body velocity per step
 * W               forward accel while held (no target speed; tap = micro-adjust)
 * A / D           sideways, same model, combines with W
 * S               brake to a clean stop (wins over everything)
 * release SHIFT   gravity/damping restored; momentum carries into the landing
 * SPACE           game handbrake, never read
 *
 * UObject access on the game thread only; body-level writes, never the transform.
 */

namespace
{
	constexpr double PI = 3.14159265358979323846;

	// cm/s, cm/s^2.
	constexpr double MAX_SPEED        = 10000.0;  // runaway clamp (100 m/s)
	constexpr double RISE_SPEED       = 150.0;    // climb speed held by the lift ramp
	constexpr double LIFT_ACCEL       = 1200.0;   // ramp rate up to RISE_SPEED
	constexpr double FORWARD_ACCEL    = 1500.0;   // W, per second while held
	constexpr double BRAKE_ACCEL      = 2500.0;   // S
	constexpr double STRAFE_ACCEL     = 1200.0;   // A/D, per second

	// rad/s^2 (bAccelChange = true, so inertia does not enter).
	constexpr double MAX_ACCEL        = 20.0;     // total angular accel clamp
	constexpr double LEVEL_GAIN       = 6.0;      // per rad of pitch / roll error
	constexpr double YAW_GAIN         = 4.0;      // per rad of yaw error
	constexpr double DAMP            = 2.5;      // per rad/s of angular velocity

	bool g_has_console = false;

	char g_last_line[600] = "(no output yet)";   // last debug line, survives for the crash filter
	char g_crash_log_path[MAX_PATH] = "output.crash.log";

	void debug_line(const char* fmt, ...)
	{
		char buf[512];
		va_list args;
		va_start(args, fmt);
		const int written = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
		va_end(args);

		if (written <= 0)
			return;

		strcat_s(buf, "\n");

		strncpy_s(g_last_line, buf, _TRUNCATE);
		for (int i = (int)strlen(g_last_line) - 1; i >= 0 && (g_last_line[i] == '\n' || g_last_line[i] == '\r'); --i)
			g_last_line[i] = '\0';

		if (g_has_console)
		{
			fputs(buf, stdout);
			fflush(stdout);
		}
		OutputDebugStringA(buf);
	}

	LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
	{
		const EXCEPTION_RECORD* er = ep->ExceptionRecord;

		char module_path[MAX_PATH] = "unknown-module";
		unsigned long long mod_base = 0;
		HMODULE mod = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       (LPCSTR)er->ExceptionAddress, &mod) && mod)
		{
			GetModuleFileNameA(mod, module_path, MAX_PATH);
			mod_base = (unsigned long long)(uintptr_t)mod;
		}

		char line[1200];
		snprintf(line, sizeof(line),
		         "[crash] code=0x%08lX addr=%p module=%s+0x%llX | last=%s\r\n",
		         er->ExceptionCode, er->ExceptionAddress, module_path,
		         (unsigned long long)((unsigned long long)(uintptr_t)er->ExceptionAddress - mod_base),
		         g_last_line);

		FILE* f = nullptr;
		if (fopen_s(&f, g_crash_log_path, "a") == 0 && f)
		{
			fputs(line, f);
			fclose(f);
		}
		OutputDebugStringA(line);

		return EXCEPTION_EXECUTE_HANDLER;
	}

	void init_crash_log(HMODULE self)
	{
		char path[MAX_PATH] = {};
		if (self && GetModuleFileNameA(self, path, MAX_PATH))
			snprintf(g_crash_log_path, sizeof(g_crash_log_path), "%s.crash.log", path);

		SetUnhandledExceptionFilter(crash_filter);
	}

	// Anchor every native function once at startup: NOT FOUND = game update
	// wants a table refresh; a moved RVA with a match = code slid, signature holds.
	void verify_signatures()
	{
		const uint8_t* image_base = (const uint8_t*)GetModuleHandleW(nullptr);
		int found = 0;
		constexpr int total = (int)(sizeof(signatures::TABLE) / sizeof(signatures::TABLE[0]));

		for (const signatures::known_signature& sig : signatures::TABLE)
		{
			signatures::pattern p;
			if (!signatures::parse(sig.m_text, p))
			{
				debug_line("[sig] %-25s parse error", sig.m_name);
				continue;
			}

			const uint8_t* hit = signatures::find_in_image(p);
			if (!hit)
			{
				debug_line("[sig] %-25s NOT FOUND - game update?", sig.m_name);
				continue;
			}

			++found;
			const uint32_t rva = (uint32_t)(hit - image_base);
			if (rva == sig.m_rva)
				debug_line("[sig] %-25s ok rva 0x%X", sig.m_name, rva);
			else
				debug_line("[sig] %-25s moved rva 0x%X (table 0x%X)", sig.m_name, rva, sig.m_rva);
		}

		debug_line("[sig] %d/%d signatures resolved", found, total);
	}

	bool is_key_down(int virtual_key)
	{
		return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
	}

	double get_axis(int positive_key, int negative_key)
	{
		return (is_key_down(positive_key) ? 1.0 : 0.0) - (is_key_down(negative_key) ? 1.0 : 0.0);
	}

	double wrap_axis(double angle)
	{
		// Same result as FRotator::NormalizeAxis, kept local.
		angle = std::fmod(angle, 360.0);
		if (angle < 0.0)
			angle += 360.0;
		return angle > 180.0 ? angle - 360.0 : angle;
	}

	// UE rotation matrix columns, written out to avoid linking GetActorForwardVector etc.
	void get_basis(const unreal::FRotator& rotation, unreal::FVector& forward, unreal::FVector& right, unreal::FVector& up)
	{
		const double pitch = rotation.Pitch * PI / 180.0;
		const double yaw   = rotation.Yaw   * PI / 180.0;

		const double cp = std::cos(pitch), sp = std::sin(pitch);
		const double cy = std::cos(yaw),   sy = std::sin(yaw);

		forward = { cp * cy, cp * sy, sp };
		right   = { -sy,     cy,      0.0 };
		up      = { -sp * cy, -sp * sy, cp };
	}

	struct state
	{
		unreal::FRotator m_takeoff_attitude{};   // latched at Engage, held all flight
		unreal::FVector m_kin_vel{};             // our integrated velocity
		unreal::AVehicleBase* m_vehicle{ nullptr };
		unreal::UPrimitiveComponent* m_mesh{ nullptr };
		double m_saved_spring_downforce{ 0.0 }; // AVS state saved at Engage,
		double m_saved_base_linear_drag{ 0.0 }; // restored on Disengage
		double m_saved_default_linear_drag{ 0.0 };
		double m_saved_linear_damping{ 0.0 };
		ULONGLONG m_last_step_ms{ 0 };          // integrator dt
		bool m_saved_dynamic_air_drag{ false };
		bool m_engaged{ false };
		bool m_saved_gravity{ true };
		bool m_saved_block_down_force{ false };
	};

	state g_state;

	// g_world: level-transition detector, not a cache; the live world is re-fetched every tick.
	unreal::UWorld* g_world       = nullptr;
	HWND         g_game_wnd     = nullptr;
	WNDPROC      g_orig_wnd_proc = nullptr;

	constexpr UINT_PTR TICK_TIMER_ID  = 0x52494445; // arbitrary, non-zero
	constexpr UINT     TICK_PERIOD_MS = 8;          // 8 ms cadence

	unreal::APlayerController* get_controller(unreal::UWorld* world)
	{
		unreal::UGameInstance* game_instance = static_cast<unreal::UGameInstance*>(world->OwningGameInstance);
		if (!game_instance || game_instance->LocalPlayers.Num <= 0)
			return nullptr;

		unreal::ULocalPlayer* local = static_cast<unreal::ULocalPlayer*>(game_instance->LocalPlayers.at(0));
		return static_cast<unreal::APlayerController*>(local->PlayerController);
	}

	unreal::AVehicleBase* get_vehicle(unreal::APlayerController* controller)
	{
		unreal::APawn* pawn = controller ? static_cast<unreal::APawn*>(controller->Pawn) : nullptr;

		// Matched by name through the class chain.
		if (!pawn || !unreal::is_a(pawn, L"BP_VehicleBase_C"))
			return nullptr;

		return static_cast<unreal::AVehicleBase*>(pawn);
	}

	void engage(unreal::AVehicleBase* vehicle, unreal::UPrimitiveComponent* mesh)
	{
		g_state.m_saved_gravity        = unreal::is_gravity_enabled(mesh);
		g_state.m_saved_block_down_force = vehicle->BlockDownwardForceInAir;

		// Park drag/damping at zero: they would ripple the step between overwrites.
		g_state.m_saved_spring_downforce    = vehicle->SpringDownforce;
		g_state.m_saved_base_linear_drag     = vehicle->BaseLinearDrag;
		g_state.m_saved_default_linear_drag  = vehicle->DefaultLinearDrag;
		g_state.m_saved_dynamic_air_drag     = vehicle->DynamicAirDrag;
		g_state.m_saved_linear_damping      = unreal::get_linear_damping(mesh);
		vehicle->SpringDownforce   = 0.0;
		vehicle->BaseLinearDrag    = 0.0;
		vehicle->DefaultLinearDrag = 0.0;
		vehicle->DynamicAirDrag    = false;
		unreal::set_linear_damping(mesh, 0.0f);

		// Seed integrator with body velocity; latch the pose held for the whole flight.
		const unreal::FVector velocity_0 = unreal::get_physics_linear_velocity(mesh, {});
		g_state.m_kin_vel = velocity_0;
		g_state.m_takeoff_attitude = unreal::k2_get_actor_rotation(vehicle);
		g_state.m_last_step_ms = GetTickCount64();

		unreal::set_enable_gravity(mesh, false);
		vehicle->BlockDownwardForceInAir = true;

		debug_line("engage (sim stays on, gravity was %d, blockDownForce was %d, pose=%.1f/%.1f/%.1f, vel=(%.0f %.0f %.0f))",
			g_state.m_saved_gravity ? 1 : 0, g_state.m_saved_block_down_force ? 1 : 0,
			g_state.m_takeoff_attitude.Pitch, g_state.m_takeoff_attitude.Yaw, g_state.m_takeoff_attitude.Roll,
			g_state.m_kin_vel.X, g_state.m_kin_vel.Y, g_state.m_kin_vel.Z);

		g_state.m_vehicle  = vehicle;
		g_state.m_mesh     = mesh;
		g_state.m_engaged = true;
	}

	void disengage(unreal::AVehicleBase* current_vehicle, unreal::UPrimitiveComponent* current_mesh)
	{
		if (!g_state.m_engaged)
			return;

		// Pointers may be stale after respawn/swap; restoring into freed objects crashes.
		const bool stale = (g_state.m_vehicle != current_vehicle) || (g_state.m_mesh != current_mesh);
		if (stale)
		{
			debug_line("disengage: saved pointers stale (vehicle %p -> %p), skipping restore",
				(void*)g_state.m_vehicle, (void*)current_vehicle);
			g_state = state{};
			return;
		}

		if (g_state.m_mesh)
		{
			// No velocity restore: body already carries our last m_kin_vel.
			unreal::set_enable_gravity(g_state.m_mesh, g_state.m_saved_gravity);
			unreal::set_linear_damping(g_state.m_mesh, static_cast<float>(g_state.m_saved_linear_damping));
			unreal::set_physics_angular_velocity_in_radians(g_state.m_mesh, { 0.0, 0.0, 0.0 }, false, {});
			unreal::wake_rigid_body(g_state.m_mesh, {});
		}

		if (g_state.m_vehicle)
		{
			g_state.m_vehicle->BlockDownwardForceInAir = g_state.m_saved_block_down_force;
			g_state.m_vehicle->SpringDownforce   = g_state.m_saved_spring_downforce;
			g_state.m_vehicle->BaseLinearDrag    = g_state.m_saved_base_linear_drag;
			g_state.m_vehicle->DefaultLinearDrag = g_state.m_saved_default_linear_drag;
			g_state.m_vehicle->DynamicAirDrag    = g_state.m_saved_dynamic_air_drag;
		}

		debug_line("disengage (body already at vel=(%.0f %.0f %.0f), restored gravity=%d, blockDownForce=%d)",
			g_state.m_kin_vel.X, g_state.m_kin_vel.Y, g_state.m_kin_vel.Z,
			g_state.m_saved_gravity ? 1 : 0, g_state.m_saved_block_down_force ? 1 : 0);

		g_state = state{};
	}

	void step(unreal::AVehicleBase* vehicle, unreal::UPrimitiveComponent* mesh, unreal::APlayerController* controller)
	{
		const unreal::FName bone;                       // NAME_None
		const unreal::FRotator control = unreal::get_control_rotation(controller);
		const unreal::FRotator actor   = unreal::k2_get_actor_rotation(vehicle);

		// Clamp dt: debugger pauses must not integrate a huge jump.
		const ULONGLONG now_ms = GetTickCount64();
		double dt = (now_ms - g_state.m_last_step_ms) * 0.001;
		g_state.m_last_step_ms = now_ms;
		if (dt > 0.1)
			dt = 0.1;
		if (dt <= 0.0)
			dt = 0.008;

		// Re-zero every tick: the Blueprint re-asserts gravity, down-force and drag.
		unreal::set_enable_gravity(mesh, false);
		vehicle->BlockDownwardForceInAir = true;
		vehicle->SpringDownforce   = 0.0;
		vehicle->BaseLinearDrag    = 0.0;
		vehicle->DefaultLinearDrag = 0.0;
		vehicle->DynamicAirDrag    = false;
		unreal::set_linear_damping(mesh, 0.0f);

		// Forward from camera yaw only; pitch ignored so looking down does not dive.
		const double cam_yaw = control.Yaw * PI / 180.0;
		const double fx = std::cos(cam_yaw), fy = std::sin(cam_yaw);

		double vel_x = g_state.m_kin_vel.X;
		double vel_y = g_state.m_kin_vel.Y;
		double vel_z = g_state.m_kin_vel.Z;

		if (is_key_down('S'))
		{
			const double horiz_speed = std::sqrt(vel_x * vel_x + vel_y * vel_y);
			if (horiz_speed <= 20.0)
			{
				vel_x = 0.0;
				vel_y = 0.0;
			}
			else
			{
				double brake_step = BRAKE_ACCEL * dt;
				if (brake_step > horiz_speed - 20.0)
					brake_step = horiz_speed - 20.0;
				const double scale = (horiz_speed - brake_step) / horiz_speed;
				vel_x *= scale;
				vel_y *= scale;
			}
		}
		else
		{
			if (is_key_down('W'))
			{
				vel_x += fx * FORWARD_ACCEL * dt;
				vel_y += fy * FORWARD_ACCEL * dt;
			}

			const double strafe = get_axis('D', 'A');
			if (strafe != 0.0)
			{
				vel_x += -fy * strafe * STRAFE_ACCEL * dt;
				vel_y +=  fx * strafe * STRAFE_ACCEL * dt;
			}

			const double new_speed = std::sqrt(vel_x * vel_x + vel_y * vel_y);
			if (new_speed > MAX_SPEED)
			{
				const double scale = MAX_SPEED / new_speed;
				vel_x *= scale;
				vel_y *= scale;
			}
		}

		if (vel_z < RISE_SPEED)
		{
			vel_z += LIFT_ACCEL * dt;
			if (vel_z > RISE_SPEED)
				vel_z = RISE_SPEED;
		}

		g_state.m_kin_vel.X = vel_x;
		g_state.m_kin_vel.Y = vel_y;
		g_state.m_kin_vel.Z = vel_z;

		// Absolute overwrite of the body velocity; never the transform - that races
		// the game thread.
		unreal::wake_rigid_body(mesh, bone);
		unreal::set_physics_linear_velocity(mesh, { vel_x, vel_y, vel_z }, false, bone);

		// Error against the latched takeoff pose, not level or camera yaw.
		// UE positive pitch is a NEGATIVE right-hand rotation about Right (roll
		// mirrors it), so pitch/roll torque opposes the error sign. Yaw about +Z is as-is.
		const double pitch_err = (g_state.m_takeoff_attitude.Pitch - actor.Pitch) * PI / 180.0;
		const double roll_err  = (g_state.m_takeoff_attitude.Roll  - actor.Roll)  * PI / 180.0;
		const double yaw_err   =  wrap_axis(g_state.m_takeoff_attitude.Yaw - actor.Yaw) * PI / 180.0;

		unreal::FVector fwd, right, up;
		get_basis(actor, fwd, right, up);

		const unreal::FVector ang_vel = unreal::get_physics_angular_velocity_in_radians(mesh, bone);

		unreal::FVector torque;
		torque.X = -right.X * (pitch_err * LEVEL_GAIN) - fwd.X * (roll_err * LEVEL_GAIN) - ang_vel.X * DAMP;
		torque.Y = -right.Y * (pitch_err * LEVEL_GAIN) - fwd.Y * (roll_err * LEVEL_GAIN) - ang_vel.Y * DAMP;
		torque.Z = -right.Z * (pitch_err * LEVEL_GAIN) - fwd.Z * (roll_err * LEVEL_GAIN) - ang_vel.Z * DAMP
		         + yaw_err * YAW_GAIN;

		const double magnitude = std::sqrt(torque.X * torque.X + torque.Y * torque.Y + torque.Z * torque.Z);
		if (magnitude > MAX_ACCEL)
		{
			const double scale = MAX_ACCEL / magnitude;
			torque.X *= scale;
			torque.Y *= scale;
			torque.Z *= scale;
		}

		unreal::add_torque_in_radians(mesh, torque, bone, true);
	}

	// Game-thread pump at >= 8 ms spacing; all UObject access stays in this path.
	void tick()
	{
		static ULONGLONG last_tick = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last_tick < TICK_PERIOD_MS)
			return;
		last_tick = now;

		// Re-fetch every tick; the level swap can leave it null or half-initialized.
		unreal::UWorld* world = unreal::get_world();
		if (!world || !world->PersistentLevel || !world->OwningGameInstance)
			return;

		if (world != g_world)
		{
			disengage(nullptr, nullptr);   // old-world pointers, stale now
			g_world = world;
			debug_line("world changed -> %p, re-acquired", (void*)world);
		}

		unreal::APlayerController*   controller = get_controller(world);
		unreal::AVehicleBase*   vehicle    = get_vehicle(controller);
		unreal::UPrimitiveComponent* mesh       = vehicle ? static_cast<unreal::UPrimitiveComponent*>(vehicle->VehicleMesh) : nullptr;

		if (!is_key_down(VK_SHIFT) || !vehicle || !mesh || !controller)
		{
			disengage(vehicle, mesh);
			return;
		}

		if (!g_state.m_engaged || g_state.m_vehicle != vehicle)
		{
			disengage(vehicle, mesh);
			engage(vehicle, mesh);
		}

		step(vehicle, mesh, controller);
	}

	LRESULT CALLBACK hook_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (msg == WM_TIMER && wp == TICK_TIMER_ID)
		{
			tick();
			return 0;
		}

		tick();   // backup pump in case the timer message is coalesced away

		// g_orig_wnd_proc can be null for messages dispatched before the assignment below.
		if (g_orig_wnd_proc)
			return CallWindowProcW(g_orig_wnd_proc, hwnd, msg, wp, lp);
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	struct window_search
	{
		HWND m_found{ nullptr };
		DWORD m_pid{ 0 };
		bool m_got_unreal_class{ false };
	};

	BOOL CALLBACK find_game_window_proc(HWND hwnd, LPARAM lparam)
	{
		window_search* s = reinterpret_cast<window_search*>(lparam);

		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid != s->m_pid || !IsWindowVisible(hwnd))
			return TRUE;

		char class_name[64] = {};
		GetClassNameA(hwnd, class_name, sizeof(class_name) - 1);

		// Prefer UE's window class; any visible window of this process as fallback
		// (the debug console lives in conhost, excluded by the PID filter).
		if (strcmp(class_name, "UnrealWindow") == 0)
		{
			s->m_found = hwnd;
			s->m_got_unreal_class = true;
			return FALSE;
		}
		if (!s->m_got_unreal_class && s->m_found == nullptr)
			s->m_found = hwnd;
		return TRUE;
	}

	HWND find_game_window()
	{
		window_search s{ nullptr, GetCurrentProcessId(), false };
		EnumWindows(find_game_window_proc, reinterpret_cast<LPARAM>(&s));
		return s.m_found;
	}

	// Subclass the game window; false if no window yet, caller retries.
	bool install_hook()
	{
		if (g_game_wnd && IsWindow(g_game_wnd))
			return true;

		HWND hwnd = find_game_window();
		if (!hwnd)
			return false;

		WNDPROC prev = reinterpret_cast<WNDPROC>(
			SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hook_wnd_proc)));
		if (!prev)
		{
			debug_line("subclass of %p failed (%lu)", (void*)hwnd, GetLastError());
			return false;
		}

		// g_orig_wnd_proc before SetTimer so the first tick already passes messages on.
		g_orig_wnd_proc = prev;
		g_game_wnd = hwnd;
		SetTimer(hwnd, TICK_TIMER_ID, TICK_PERIOD_MS, nullptr);

		debug_line("game-thread tick installed (hwnd=%p, timer=%u ms)", (void*)hwnd, TICK_PERIOD_MS);
		return true;
	}
}

namespace rv
{
DWORD main_thread(HMODULE module)
{
	init_crash_log(module);   // crash filter first, before any console exists

	FILE* console_out = nullptr;
	if (AllocConsole())
		freopen_s(&console_out, "CONOUT$", "w", stdout);
	SetConsoleTitleA("RV debug");
	g_has_console = (console_out != nullptr);

	debug_line("waiting for game window ... (build " __DATE__ " " __TIME__ ")");

	resolve_globals(debug_line);
	verify_signatures();

	bool installed = false;
	bool retried_logged = false;
	while (true)
	{
		if (!installed)
		{
			installed = install_hook();
			if (installed)
				continue;

			if (!retried_logged)
			{
				retried_logged = true;
				debug_line("no game window yet, retrying quietly ...");
			}
		}
		else if (!g_game_wnd || !IsWindow(g_game_wnd))
		{
			debug_line("game window lost, re-installing hook ...");
			g_game_wnd = nullptr;
			g_orig_wnd_proc = nullptr;
			installed = false;
		}

		Sleep(250);
	}

	return 0;
}
} // namespace rv

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)rv::main_thread, module, 0, 0);
		break;
	}

	return TRUE;
}
