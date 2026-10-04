#include <Windows.h>

#include "crash.hpp"
#include "diagnostics.hpp"
#include "hook.hpp"
#include "log.hpp"
#include "resolve.hpp"
#include "state.hpp"

/*
 * SHIFT           kinematic flight: own velocity, overwrites body velocity per step
 * Wforward accel while held (no target speed; tap = micro-adjust)
 * A / D           sideways, same model, combines with W
 * S               brake to a clean stop (wins over everything)
 * release SHIFT   gravity/damping restored; momentum carries into the landing
 * SPACE           game handbrake, never read
 *
 * This file is the orchestrator only: the flight model is in flight.hpp, the
 * window hook and game-thread pump in hook.hpp, the AVS restore path in
 * disengage() (flight.hpp), and the startup offset resolution in resolve.hpp.
 */

// user32 is not in cl.exe's default libs.
#pragma comment(lib, "user32.lib")

namespace rv
{
	namespace helper
	{
		inline void open_debug_console(HMODULE module)
		{
			// Crash filter first: it must be in place before anything else can
			// fault, and it writes to a file rather than the console anyway.
			init_crash_log(module);

			if (AllocConsole())
			{
				(void)freopen_s(&g_console_out, "CONOUT$", "w", stdout);
				HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
				DWORD mode = 0;
				if (GetConsoleMode(hOut, &mode))
					SetConsoleMode(hOut, mode & ~ENABLE_QUICK_EDIT_MODE);
			}
			SetConsoleTitleA("RV debug");

			log::debug("waiting for game window ... (build " __DATE__ " " __TIME__ ")");
		}

		inline DWORD main_thread(HMODULE module)
		{
			g_module = module;
			open_debug_console(module);

			resolve_globals();
			verify_signatures();

			bool installed = false;
			bool retried_logged = false;
			while (!g_shutdown.load(std::memory_order_acquire))
			{
				if (!installed)
				{
					installed = install_hook();
					if (installed)
						continue;

					if (!retried_logged)
					{
						retried_logged = true;
						log::debug("no game window yet, retrying quietly ...");
					}
				}
				else if (!g_game_wnd || !IsWindow(g_game_wnd))
				{
					log::debug("game window lost, re-installing hook ...");
					// Drop the stale hook bookkeeping only. Unsubclassing here
					// would be a cross-thread call into a window that is already
					// gone, and shutdown() re-checks IsWindow anyway.
					g_game_wnd = nullptr;
					g_orig_wnd_proc = nullptr;
					installed = false;
				}

				Sleep(250);
			}

			shutdown();
			return 0;
		}

		inline DWORD WINAPI thread_main(LPVOID param)
		{
			// DllMain still holds the loader lock when CreateThread returns, and
			// the first thing this thread does (SetUnhandledExceptionFilter,
			// GetModuleFileNameA) can need that lock. Yield long enough for
			// DllMain to return and the lock to drop before touching Win32.
			Sleep(50);
			return main_thread(reinterpret_cast<HMODULE>(param));
		}
	} // namespace helper
} // namespace rv

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
	{
		rv::helper::g_module = module;
		// The only thread we care about is the one we create below; skipping
		// the per-thread callbacks keeps the loader from re-entering us on
		// every thread that the engine spawns.
		DisableThreadLibraryCalls(module);
		rv::helper::g_main_thread = CreateThread(nullptr, 0, &rv::helper::thread_main, module, 0, nullptr);
		break;
	}
	case DLL_PROCESS_DETACH:
		// reserved != null means the process is exiting: other threads are
		// already gone and the loader lock is held, so touching the game
		// window or the vehicle from here would deadlock. A real FreeLibrary
		// arrives with reserved == null and gets the full unwind.
		if (!reserved)
			rv::helper::shutdown();
		break;
	}

	return TRUE;
}