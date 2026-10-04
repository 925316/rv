#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstdio>

namespace rv
{
	namespace helper
	{
		inline char g_crash_log_path[MAX_PATH] = "output.crash.log";
		// The game's own reporter, chained. This is a function pointer, not a
		// handle: crash_filter() calls through it below.
		inline LPTOP_LEVEL_EXCEPTION_FILTER g_prev_crash_filter = nullptr;

		inline LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
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
				"[crash] code=0x%08lX addr=%p module=%s+0x%llX\r\n",
				er->ExceptionCode, er->ExceptionAddress, module_path,
				(unsigned long long)((unsigned long long)(uintptr_t)er->ExceptionAddress - mod_base));

			FILE* f = nullptr;
			if (fopen_s(&f, g_crash_log_path, "a") == 0 && f)
			{
				fputs(line, f);
				fclose(f);
			}
			OutputDebugStringA(line);

			if (g_prev_crash_filter)
				return g_prev_crash_filter(ep);
			return EXCEPTION_EXECUTE_HANDLER;
		}

		inline void init_crash_log(HMODULE self)
		{
			char path[MAX_PATH] = {};
			if (self && GetModuleFileNameA(self, path, MAX_PATH))
				snprintf(g_crash_log_path, sizeof(g_crash_log_path), "%s.crash.log", path);

			// Chain, do not replace: the game installs its own reporter, and
		// swallowing every unhandled exception hides its crash dump.
			g_prev_crash_filter = SetUnhandledExceptionFilter(crash_filter);
		}

		// Restore the previous filter on unload so no dangling pointer stays.
		inline void remove_crash_log()
		{
			if (g_prev_crash_filter)
			{
				SetUnhandledExceptionFilter(g_prev_crash_filter);
				g_prev_crash_filter = nullptr;
			}
		}
	}
}
