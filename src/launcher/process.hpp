#pragma once

#include <Windows.h>
#include <TlHelp32.h>
#include <cwchar>

#include "config.hpp"

// Process discovery helpers. The Win32 calls stay here, but the polling
// loop takes a callable so unit tests can inject a fake poll function.
namespace launcher
{
	inline DWORD find_process_id(const wchar_t* exe_name)
	{
		if (exe_name == nullptr || exe_name[0] == L'\0')
			return 0;

		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return 0;

		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);

		DWORD pid = 0;
		if (Process32FirstW(snap, &entry))
		{
			do
			{
				if (_wcsicmp(entry.szExeFile, exe_name) == 0)
				{
					pid = entry.th32ProcessID;
					break;
				}
			} while (Process32NextW(snap, &entry));
		}

		CloseHandle(snap);
		return pid;
	}

	inline bool has_module(DWORD pid, const wchar_t* module_name, bool retry = false)
	{
		if (pid == 0 || module_name == nullptr || module_name[0] == L'\0')
			return false;

		// Retry only post-injection: the loader may still be registering.
		const int max_tries = retry ? kModuleRetryCount : 1;
		for (int attempt = 0; attempt < max_tries; ++attempt)
		{
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
			if (snap != INVALID_HANDLE_VALUE)
			{
				MODULEENTRY32W entry{};
				entry.dwSize = sizeof(entry);

				bool found = false;
				if (Module32FirstW(snap, &entry))
				{
					do
					{
						if (_wcsicmp(entry.szModule, module_name) == 0)
						{
							found = true;
							break;
						}
					} while (Module32NextW(snap, &entry));
				}

				CloseHandle(snap);
				if (found)
					return true;
			}
			Sleep(kModuleRetryDelayMs);
		}
		return false;
	}

	inline bool poll_main_window_once(DWORD pid)
	{
		HWND window = nullptr;
		while ((window = FindWindowExW(nullptr, window, nullptr, nullptr)) != nullptr)
		{
			DWORD window_pid = 0;
			GetWindowThreadProcessId(window, &window_pid);
			if (window_pid == pid && IsWindowVisible(window))
				return true;
		}
		return false;
	}

	template <typename PollFn>
	inline bool wait_for_main_window(DWORD pid, DWORD timeout_ms, PollFn&& poll_fn)
	{
		// Visible window = engine loop up = remote LoadLibrary is safe.
		const ULONGLONG deadline = GetTickCount64() + timeout_ms;
		while (GetTickCount64() < deadline)
		{
			if (poll_fn(pid))
				return true;
			Sleep(kPollIntervalMs);
		}
		return false;
	}

	inline bool wait_for_main_window(DWORD pid, DWORD timeout_ms)
	{
		return wait_for_main_window(pid, timeout_ms, poll_main_window_once);
	}
}
