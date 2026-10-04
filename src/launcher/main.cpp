/*
 * Launcher - one-click injector for output.dll.
 * find process -> verify not injected -> extract embedded DLL to %TEMP% ->
 * CreateRemoteThread + LoadLibraryW -> verify -> report.
 * Build: CMake from the repo root; dllbytes.h is generated at build time.
 */

#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cwchar>   // swprintf_s, _wcsicmp

#include "dllbytes.h" // generated: DLL_BYTES[] / DLL_SIZE

namespace
{
	// Process name only, no install path; waits for the user to start the game.
	const wchar_t* PROCESS_NAME = L"Ride-Win64-Shipping.exe";
	const wchar_t* DLL_NAME     = L"output.dll";

	void fail(const wchar_t* what, DWORD err = 0)
	{
		if (err)
			wprintf(L"[FAIL] %s (GetLastError=%lu)\n", what, err);
		else
			wprintf(L"[FAIL] %s\n", what);
		wprintf(L"Press Enter to exit...\n");
		(void)getchar();
		ExitProcess(1);
	}

	void ok(const wchar_t* what)
	{
		wprintf(L"[ OK ] %s\n", what);
	}

	DWORD find_process_id(const wchar_t* exe_name)
	{
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

	bool has_module(DWORD pid, const wchar_t* module_name, bool retry = false)
	{
		// Retry only post-injection (loader may still be registering the module);
		// the pre-inject check runs once so a clean start does not stall.
		const int max_tries = retry ? 10 : 1;
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
			Sleep(500);
		}
		return false;
	}

	bool wait_for_main_window(DWORD pid, DWORD timeout_ms)
	{
		// Visible window = engine loop up = remote LoadLibrary is safe.
		const ULONGLONG deadline = GetTickCount64() + timeout_ms;
		while (GetTickCount64() < deadline)
		{
			HWND window = nullptr;
			while ((window = FindWindowExW(nullptr, window, nullptr, nullptr)) != nullptr)
			{
				DWORD window_pid = 0;
				GetWindowThreadProcessId(window, &window_pid);
				if (window_pid == pid && IsWindowVisible(window))
					return true;
			}
			Sleep(500);
		}
		return false;
	}

	void enable_debug_privilege()
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
			return; // best effort - same-user injection usually works without it

		LUID luid{};
		if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid))
		{
			TOKEN_PRIVILEGES privileges{};
			privileges.PrivilegeCount = 1;
			privileges.Privileges[0].Luid = luid;
			privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
			AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
		}
		CloseHandle(token);
	}

	void extract_dll(wchar_t* out_path, size_t out_path_count)
	{
		wchar_t temp_dir[MAX_PATH] = {};
		if (GetTempPathW(MAX_PATH, temp_dir) == 0)
			fail(L"GetTempPathW failed", GetLastError());

		wchar_t candidate[MAX_PATH] = {};
		swprintf_s(candidate, L"%s%s", temp_dir, DLL_NAME);

		HANDLE file = CreateFileW(candidate, GENERIC_WRITE, 0, nullptr,
		                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			// Fixed path locked by a running instance - retry with a unique name.
			swprintf_s(candidate, L"%s%s_%lu.dll", temp_dir, L"output", GetCurrentProcessId());
			file = CreateFileW(candidate, GENERIC_WRITE, 0, nullptr,
			                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				fail(L"cannot create the DLL in %TEMP%", GetLastError());
		}

		DWORD written = 0;
		const BOOL wrote = WriteFile(file, DLL_BYTES, DLL_SIZE, &written, nullptr);
		CloseHandle(file);
		if (!wrote || written != DLL_SIZE)
			fail(L"WriteFile of the embedded DLL failed", GetLastError());

		wcsncpy_s(out_path, out_path_count, candidate, _TRUNCATE);
	}

	void inject(DWORD pid, const wchar_t* dll_path)
	{
		HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
		                             PROCESS_VM_WRITE | PROCESS_VM_READ,
		                             FALSE, pid);
		if (!process)
		{
			enable_debug_privilege();
			process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
			                      PROCESS_VM_WRITE | PROCESS_VM_READ,
			                      FALSE, pid);
			if (!process)
				fail(L"OpenProcess failed", GetLastError());
		}

		const SIZE_T path_bytes = (wcslen(dll_path) + 1) * sizeof(wchar_t);

		void* remote = VirtualAllocEx(process, nullptr, path_bytes,
		                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!remote)
			fail(L"VirtualAllocEx failed", GetLastError());

		SIZE_T done = 0;
		if (!WriteProcessMemory(process, remote, dll_path, path_bytes, &done) || done != path_bytes)
			fail(L"WriteProcessMemory failed", GetLastError());

		// LoadLibraryW is at the same address in every process this boot.
		auto load_library_addr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
			GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
		if (!load_library_addr)
			fail(L"GetProcAddress(LoadLibraryW) failed", GetLastError());

		HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library_addr,
		                                   remote, 0, nullptr);
		if (!thread)
			fail(L"CreateRemoteThread failed", GetLastError());

		const DWORD wait = WaitForSingleObject(thread, 15000);
		CloseHandle(thread);
		VirtualFreeEx(process, remote, 0, MEM_RELEASE);
		CloseHandle(process);

		if (wait != WAIT_OBJECT_0)
			fail(L"remote LoadLibrary did not finish in 15 s");

		if (!has_module(pid, DLL_NAME, /*retry=*/true))
			fail(L"LoadLibrary finished but output.dll is not in the target "
			     L"(remote load returned NULL - dependency missing?)");

		DeleteFileW(dll_path);   // best-effort: the loader keeps the file locked
	}
}

int wmain()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	// %hs = narrow string in a wide format (MSVC extension).
	wprintf(L"RideFlight launcher (built %hs %hs, dll %u bytes)\n",
	        __DATE__, __TIME__, static_cast<unsigned>(DLL_SIZE));

	DWORD pid = find_process_id(PROCESS_NAME);
	if (pid)
	{
		wprintf(L"[1/4] game running, pid=%lu\n", pid);
	}
	else
	{
		wprintf(L"[1/4] game not running - start it now, waiting ...\n");
		for (;;)
		{
			Sleep(500);
			pid = find_process_id(PROCESS_NAME);
			if (pid)
				break;
		}
		wprintf(L"[ OK ] game detected, pid=%lu\n", pid);
	}

	if (has_module(pid, DLL_NAME))
	{
		ok(L"output.dll already loaded - nothing to do");
		wprintf(L"Press Enter to exit...\n");
		(void)getchar();
		return 0;
	}
	wprintf(L"[2/4] not injected yet\n");

	if (!wait_for_main_window(pid, 120000))
		fail(L"no visible game window within 120 s - is the game on a login/splash screen?");
	ok(L"game window is up");

	wchar_t dll_path[MAX_PATH] = {};
	extract_dll(dll_path, MAX_PATH);
	wprintf(L"[3/4] extracted: %s\n", dll_path);

	inject(pid, dll_path);
	wprintf(L"[4/4] injected - hold Shift in-game to fly\n");

	Sleep(1500); // brief success flash before the window closes
	return 0;
}
