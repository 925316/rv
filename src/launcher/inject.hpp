#pragma once

#include <Windows.h>
#include <cwchar>

#include "config.hpp"
#include "process.hpp"

/*
 * Payload extraction and remote load. Both steps report failure instead of
 * calling ExitProcess, so the caller owns the exit code and the two steps can
 * be exercised without tearing down the test process.
 */

namespace launcher
{
	// Best effort: same-user injection usually works without it, so a failure
	// here is not an error, it just means the OpenProcess retry is our only
	// chance.
	inline void enable_debug_privilege()
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
			return;

		LUID luid{};
		// The value of SE_DEBUG_NAME, spelled wide on purpose. The macro is a
		// TEXT() wrapper, so it is only LPCWSTR when UNICODE is defined - and
		// this file is compiled by tools that do not all agree on that. An
		// explicit literal is the same string either way.
		static constexpr wchar_t kDebugPrivilege[] = L"SeDebugPrivilege";
		if (LookupPrivilegeValueW(nullptr, kDebugPrivilege, &luid))
		{
			TOKEN_PRIVILEGES privileges{};
			privileges.PrivilegeCount = 1;
			privileges.Privileges[0].Luid = luid;
			privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
			AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
		}
		CloseHandle(token);
	}

	inline bool write_payload(const wchar_t* path, const unsigned char* bytes, unsigned int size, DWORD& out_error)
	{
		HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			out_error = GetLastError();
			return false;
		}

		DWORD written = 0;
		const BOOL wrote = WriteFile(file, bytes, size, &written, nullptr);
		if (!wrote)
			out_error = GetLastError();
		else if (written != size)
			out_error = ERROR_WRITE_FAULT;
		CloseHandle(file);
		return wrote && written == size;
	}

	// Drops the embedded DLL next to the temp dir. The fixed name is tried
	// first; a running instance holds a lock on it, so a PID-suffixed name is
	// the fallback rather than an error.
	inline bool extract_dll(const unsigned char* bytes, unsigned int size,
		wchar_t* out_path, size_t out_path_count, DWORD& out_error)
	{
		wchar_t temp_dir[MAX_PATH] = {};
		if (GetTempPathW(MAX_PATH, temp_dir) == 0)
		{
			out_error = GetLastError();
			return false;
		}

		wchar_t candidate[MAX_PATH] = {};
		swprintf_s(candidate, L"%s%s", temp_dir, kDllName);

		if (!write_payload(candidate, bytes, size, out_error))
		{
			swprintf_s(candidate, L"%s%s_%lu.dll", temp_dir, kDllBaseName, GetCurrentProcessId());
			if (!write_payload(candidate, bytes, size, out_error))
				return false;
		}

		wcsncpy_s(out_path, out_path_count, candidate, _TRUNCATE);
		return true;
	}

	inline bool inject(DWORD pid, const wchar_t* dll_path, DWORD& out_error)
	{
		const DWORD access = PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION
			| PROCESS_VM_WRITE | PROCESS_VM_READ;

		HANDLE process = OpenProcess(access, FALSE, pid);
		if (!process)
		{
			enable_debug_privilege();
			process = OpenProcess(access, FALSE, pid);
			if (!process)
			{
				out_error = GetLastError();
				return false;
			}
		}

		const SIZE_T path_bytes = (wcslen(dll_path) + 1) * sizeof(wchar_t);

		void* remote = VirtualAllocEx(process, nullptr, path_bytes,
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!remote)
		{
			out_error = GetLastError();
			CloseHandle(process);
			return false;
		}

		SIZE_T done = 0;
		const BOOL wrote = WriteProcessMemory(process, remote, dll_path, path_bytes, &done);
		if (!wrote || done != path_bytes)
		{
			out_error = wrote ? ERROR_WRITE_FAULT : GetLastError();
			VirtualFreeEx(process, remote, 0, MEM_RELEASE);
			CloseHandle(process);
			return false;
		}

		// LoadLibraryW is at the same address in every process this boot.
		// The void* hop is deliberate: GetProcAddress hands back a FARPROC, and
		// casting one function-pointer type straight to another trips
		// -Wcast-function-type-mismatch even though the target really is
		// LPTHREAD_START_ROUTINE(LPCWSTR).
		auto load_library_addr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
			reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
		if (!load_library_addr)
		{
			out_error = GetLastError();
			VirtualFreeEx(process, remote, 0, MEM_RELEASE);
			CloseHandle(process);
			return false;
		}

		HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library_addr,
			remote, 0, nullptr);
		if (!thread)
		{
			out_error = GetLastError();
			VirtualFreeEx(process, remote, 0, MEM_RELEASE);
			CloseHandle(process);
			return false;
		}

		const DWORD wait = WaitForSingleObject(thread, kRemoteThreadWaitMs);
		CloseHandle(thread);
		VirtualFreeEx(process, remote, 0, MEM_RELEASE);
		CloseHandle(process);

		if (wait != WAIT_OBJECT_0)
		{
			out_error = wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : ERROR_TIMEOUT;
			return false;
		}

		if (!has_module(pid, kDllName, /*retry=*/true))
		{
			// The remote load returned but the module is absent: a dependency
			// the target cannot satisfy is the usual cause.
			out_error = ERROR_MOD_NOT_FOUND;
			return false;
		}

		DeleteFileW(dll_path);   // best-effort: the loader keeps the file locked
		return true;
	}
}