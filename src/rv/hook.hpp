#pragma once

#include <Windows.h>
#include <atomic>
#include <cstring>

#include "crash.hpp"
#include "flight.hpp"
#include "log.hpp"
#include "state.hpp"
#include "unreal.hpp"

/*
 * Window hook and the game-thread pump. The engine only touches UObjects from
 * its own thread, so the tick is driven by a timer on the game's window rather
 * than by a thread of our own: a WM_TIMER is delivered on the game thread,
 * which is what makes the body writes legal.
 *
 * Everything installed here is RAII-in-reverse: shutdown() puts back exactly
 * what install_hook() took, so an unload does not leave a subclassed wndproc
 * pointing into a freed module.
 */

namespace rv
{
	namespace helper
	{
		// Game-thread pump at >= 8 ms spacing; all UObject access stays in this path.
		inline void tick_body()
		{
			static ULONGLONG last_tick = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - last_tick < TICK_PERIOD_MS)
				return;
			last_tick = now;

			// GWorld is re-read every tick and the slot can momentarily hold whatever
			// the game just wrote into it. The crash filter terminates the
			// process, so an unguarded deref here is a game kill, not a log
			// line: readable() rejects a wild address, is_valid() rejects a
			// stale-but-mapped world.
			unreal::UWorld* world = unreal::get_world();
			if (!world || !unreal::readable(world, sizeof(unreal::UWorld)))
				return;
			if (!unreal::is_valid(world))
			{
				// The hot path depends on GObjects resolving, which it did not
				// before. One-shot line, or a bad GObjects RVA looks exactly like
				// "the cheat does nothing".
				static bool reported = false;
				log::debug_once(reported, "world %p rejected by the GObjects lookup (rva 0x%X) - tick is a no-op",
					(void*)world, (uint32_t)unreal::g_world_rva);
				return;
			}
			if (!world->PersistentLevel || !world->OwningGameInstance)
				return;

			if (world != g_world)
			{
				disengage();   // restores whatever of the old world is still live
				g_world = world;
				log::debug("world changed -> %p, re-acquired", (void*)world);
			}

			unreal::APlayerController* controller = get_controller(world);
			unreal::AVehicleBase* vehicle = get_vehicle(controller);
			unreal::UPrimitiveComponent* mesh = vehicle ? static_cast<unreal::UPrimitiveComponent*>(vehicle->VehicleMesh) : nullptr;

			if (!is_key_down(VK_SHIFT) || !vehicle || !mesh || !controller)
			{
				disengage();
				return;
			}

			// Same key as the restore gate in disengage(): if VehicleMesh swaps under a
			// held Shift, step() is about to write the new mesh while we are still
			// engaged against the old one - and the restore would then skip it.
			if (!g_state.m_engaged || g_state.m_vehicle != vehicle || g_state.m_mesh != mesh)
			{
				disengage();
				engage(vehicle, mesh);
				if (!g_state.m_engaged)
					return;   // engage refused: nothing is latched, so do not step
			}

			step(vehicle, mesh, controller);
		}

		// Re-entrancy guard. process_event runs Blueprint, Blueprint can pump
		// messages, and that lands back here on this same thread. The 8 ms
		// filter in tick_body is a RATE limit, not a re-entrancy guard: one tick
		// that runs longer than the period, or a clock step, lets the recursion
		// straight through.
		inline void tick()
		{
			static bool in_tick = false;
			if (in_tick)
				return;

			in_tick = true;
			tick_body();
			in_tick = false;
		}

		inline LRESULT CALLBACK hook_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			if (msg == WM_TIMER && wp == TICK_TIMER_ID)
			{
				tick();
				return 0;
			}

			tick();   // backup pump in case the timer message is coalesced away

			// Belt and braces: install_hook publishes g_orig_wnd_proc BEFORE the subclass
			// goes in, so this cannot be null in practice. If it ever were, the
			// DefWindowProcW fallback would silently eat engine messages instead of
			// failing loudly.
			if (g_orig_wnd_proc)
				return CallWindowProcW(g_orig_wnd_proc, hwnd, msg, wp, lp);
			return DefWindowProcW(hwnd, msg, wp, lp);
		}

		struct window_search
		{
			HWND  m_found{ nullptr };
			DWORD m_pid{ 0 };
			bool  m_got_unreal_class{ false };
		};

		inline BOOL CALLBACK find_game_window_proc(HWND hwnd, LPARAM lparam)
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

		inline HWND find_game_window()
		{
			window_search s{ nullptr, GetCurrentProcessId(), false };
			EnumWindows(find_game_window_proc, reinterpret_cast<LPARAM>(&s));
			return s.m_found;
		}

		// Subclass the game window; false if no window yet, caller retries.
		inline bool install_hook()
		{
			if (g_game_wnd && IsWindow(g_game_wnd))
				return true;

			HWND hwnd = find_game_window();
			if (!hwnd)
				return false;

			// g_orig_wnd_proc has to be live BEFORE the subclass goes in: a message can
			// arrive the instant SetWindowLongPtrW returns, and a null original
			// would route it to DefWindowProcW instead of the engine.
			WNDPROC prev = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
			if (!prev)
			{
				log::debug("no current wndproc on %p (%lu)", (void*)hwnd, GetLastError());
				return false;
			}
			g_orig_wnd_proc = prev;

			prev = reinterpret_cast<WNDPROC>(
				SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hook_wnd_proc)));
			if (!prev)
			{
				g_orig_wnd_proc = nullptr;
				log::debug("subclass of %p failed (%lu)", (void*)hwnd, GetLastError());
				return false;
			}

			g_game_wnd = hwnd;
			if (!SetTimer(hwnd, TICK_TIMER_ID, TICK_PERIOD_MS, nullptr))
			{
				log::debug("SetTimer on %p failed (%lu)", (void*)hwnd, GetLastError());
				SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prev));
				g_orig_wnd_proc = nullptr;
				g_game_wnd = nullptr;
				return false;
			}

			log::debug("game-thread tick installed (hwnd=%p, timer=%u ms)", (void*)hwnd, TICK_PERIOD_MS);
			return true;
		}

		// Reverse of install_hook, in the reverse order. Idempotent: main_thread
		// reaching the end of its loop and the DLL being unloaded both land here,
		// and whichever gets there first wins.
		inline void shutdown()
		{
			bool expected = false;
			if (!g_cleanup_done.compare_exchange_strong(expected, true))
				return;   // the other path already unwound everything

			// Kill the timer before taking the wndproc away: a WM_TIMER already
			// queued would re-enter tick() on a half-uninstalled hook.
			if (g_game_wnd && IsWindow(g_game_wnd))
			{
				KillTimer(g_game_wnd, TICK_TIMER_ID);
				if (g_orig_wnd_proc)
				{
					SetWindowLongPtrW(g_game_wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wnd_proc));
					log::debug("hook removed from %p", (void*)g_game_wnd);
				}
			}
			g_orig_wnd_proc = nullptr;
			g_game_wnd = nullptr;

			// Put the vehicle back while the code that drives it is still mapped.
			disengage();
			g_world = nullptr;

			// Only after the restores: this is what would have caught them.
			remove_crash_log();

			if (g_console_out)
			{
				fclose(g_console_out);
				g_console_out = nullptr;
				FreeConsole();
			}
		}
	}
}