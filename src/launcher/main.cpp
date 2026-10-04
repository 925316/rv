/*
 * Launcher: finds the game process and injects output.dll via remote
 * LoadLibraryW. dllbytes.h is generated at build time.
 *
 * Usage: launcher.exe [target-exe]   (default: Ride-Win64-Shipping.exe)
 *        launcher.exe --help
 *
 * The steps live in process.hpp (discovery) and inject.hpp (extract + load);
 * this file is the flow and the user-facing reporting.
 */

#include <Windows.h>
#include <cstdio>
#include <cwchar>

#include "config.hpp"
#include "dllbytes.h" // generated: DLL_BYTES[] / DLL_SIZE
#include "inject.hpp"
#include "log.hpp"
#include "process.hpp"

using rv::log::debug;

namespace
{
	void fail(launcher::ExitCode code, const wchar_t* what, DWORD err = 0)
	{
		if (err)
			debug(L"[FAIL] %s (GetLastError=%lu)", what, err);
		else
			debug(L"[FAIL] %s", what);
		debug(L"Press Enter to exit...");
		(void)getchar();
		ExitProcess(static_cast<int>(code));
	}

	void ok(const wchar_t* what)
	{
		debug(L"[ OK ] %s", what);
	}

	void print_usage()
	{
		debug(L"RV launcher");
		debug(L"  launcher.exe [target-exe]");
		debug(L"  target-exe defaults to %s", launcher::kDefaultTargetExe);
	}
}

int wmain(int argc, wchar_t* argv[])
{
	if (argc >= 2 && launcher::is_help_arg(argv[1]))
	{
		print_usage();
		return static_cast<int>(launcher::ExitCode::Success);
	}

	const wchar_t* target_exe = launcher::resolve_target_exe(argc, argv);

	// %hs = narrow string in a wide format (MSVC extension).
	debug(L"RV launcher (built %hs %hs, dll %u bytes, target %s)",
		__DATE__, __TIME__, static_cast<unsigned>(DLL_SIZE), target_exe);

	DWORD pid = launcher::find_process_id(target_exe);
	if (pid)
	{
		debug(L"[1/4] game running, pid=%lu", pid);
	}
	else
	{
		debug(L"[1/4] %s not running - start it now, waiting ...", target_exe);
		for (;;)
		{
			Sleep(launcher::kPollIntervalMs);
			pid = launcher::find_process_id(target_exe);
			if (pid)
				break;
		}
		debug(L"[ OK ] game detected, pid=%lu", pid);
	}

	if (launcher::has_module(pid, launcher::kDllName))
	{
		ok(L"output.dll already loaded - nothing to do");
		debug(L"Press Enter to exit...");
		(void)getchar();
		return static_cast<int>(launcher::ExitCode::Success);
	}
	debug(L"[2/4] not injected yet");

	if (!launcher::wait_for_main_window(pid, launcher::kWindowTimeoutMs))
		fail(launcher::ExitCode::NotFound,
			L"no visible game window in time - is the game on a login/splash screen?");
	ok(L"game window is up");

	wchar_t dll_path[MAX_PATH] = {};
	DWORD    err = 0;
	if (!launcher::extract_dll(DLL_BYTES, DLL_SIZE, dll_path, MAX_PATH, err))
		fail(launcher::ExitCode::Fatal, L"cannot write the embedded DLL to %TEMP%", err);
	debug(L"[3/4] extracted: %s", dll_path);

	if (!launcher::inject(pid, dll_path, err))
	{
		if (err == ERROR_MOD_NOT_FOUND)
			fail(launcher::ExitCode::Fatal,
				L"remote load finished but output.dll is not in the target "
				L"(dependency missing?)");
		if (err == WAIT_TIMEOUT)
			fail(launcher::ExitCode::Fatal, L"remote LoadLibrary did not finish in time");
		fail(launcher::ExitCode::Fatal, L"injection failed", err);
	}
	debug(L"[4/4] injected - hold Shift in-game to fly");

	Sleep(1500); // brief success flash before the window closes
	return static_cast<int>(launcher::ExitCode::Success);
}