/*
 * RideFlightLauncher - one-click injector for RideFlight.dll.
 * find process -> verify not injected -> extract embedded DLL to %TEMP% ->
 * CreateRemoteThread + LoadLibraryW -> verify -> report.
 * Build: Launcher\build.bat -> Launcher\build\RideFlightLauncher.exe
 * (DLL bytes embedded as dllbytes.h by the build).
 */

#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cwchar>   // swprintf_s, _wcsicmp

#include "dllbytes.h" // generated: kDllBytes[] / kDllSize

namespace
{
    // Process name only, no install path; waits for the user to start the game.
    const wchar_t* kProcessName = L"Ride-Win64-Shipping.exe";
    const wchar_t* kDllName     = L"RideFlight.dll";

    void Fail(const wchar_t* What, DWORD Err = 0)
    {
        if (Err)
            wprintf(L"[FAIL] %s (GetLastError=%lu)\n", What, Err);
        else
            wprintf(L"[FAIL] %s\n", What);
        wprintf(L"Press Enter to exit...\n");
        (void)getchar();
        ExitProcess(1);
    }

    void Ok(const wchar_t* What)
    {
        wprintf(L"[ OK ] %s\n", What);
    }

    DWORD FindProcessId(const wchar_t* ExeName)
    {
        HANDLE Snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (Snap == INVALID_HANDLE_VALUE)
            return 0;

        PROCESSENTRY32W Entry{};
        Entry.dwSize = sizeof(Entry);

        DWORD Pid = 0;
        if (Process32FirstW(Snap, &Entry))
        {
            do
            {
                if (_wcsicmp(Entry.szExeFile, ExeName) == 0)
                {
                    Pid = Entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(Snap, &Entry));
        }

        CloseHandle(Snap);
        return Pid;
    }

    bool HasModule(DWORD Pid, const wchar_t* ModuleName, bool bRetry = false)
    {
        // Retry only post-injection (loader may still be registering the module);
        // the pre-inject check runs once so a clean start does not stall.
        const int MaxTries = bRetry ? 10 : 1;
        for (int Try = 0; Try < MaxTries; ++Try)
        {
            HANDLE Snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, Pid);
            if (Snap != INVALID_HANDLE_VALUE)
            {
                MODULEENTRY32W Entry{};
                Entry.dwSize = sizeof(Entry);

                bool Found = false;
                if (Module32FirstW(Snap, &Entry))
                {
                    do
                    {
                        if (_wcsicmp(Entry.szModule, ModuleName) == 0)
                        {
                            Found = true;
                            break;
                        }
                    } while (Module32NextW(Snap, &Entry));
                }

                CloseHandle(Snap);
                if (Found)
                    return true;
            }
            Sleep(500);
        }
        return false;
    }

    bool WaitForMainWindow(DWORD Pid, DWORD TimeoutMs)
    {
        // Visible window = engine loop up = remote LoadLibrary is safe.
        const ULONGLONG Deadline = GetTickCount64() + TimeoutMs;
        while (GetTickCount64() < Deadline)
        {
            HWND Window = nullptr;
            while ((Window = FindWindowExW(nullptr, Window, nullptr, nullptr)) != nullptr)
            {
                DWORD WindowPid = 0;
                GetWindowThreadProcessId(Window, &WindowPid);
                if (WindowPid == Pid && IsWindowVisible(Window))
                    return true;
            }
            Sleep(500);
        }
        return false;
    }

    void EnableDebugPrivilege()
    {
        HANDLE Token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
            return; // best effort - same-user injection usually works without it

        LUID Luid{};
        if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &Luid))
        {
            TOKEN_PRIVILEGES Privileges{};
            Privileges.PrivilegeCount = 1;
            Privileges.Privileges[0].Luid = Luid;
            Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(Token, FALSE, &Privileges, 0, nullptr, nullptr);
        }
        CloseHandle(Token);
    }

    void ExtractDll(wchar_t* OutPath, size_t OutPathCount)
    {
        wchar_t TempDir[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, TempDir) == 0)
            Fail(L"GetTempPathW failed", GetLastError());

        wchar_t Candidate[MAX_PATH] = {};
        swprintf_s(Candidate, L"%s%s", TempDir, kDllName);

        HANDLE File = CreateFileW(Candidate, GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (File == INVALID_HANDLE_VALUE)
        {
            // Fixed path locked by a running instance - retry with a unique name.
            swprintf_s(Candidate, L"%s%s_%lu.dll", TempDir, L"RideFlight", GetCurrentProcessId());
            File = CreateFileW(Candidate, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (File == INVALID_HANDLE_VALUE)
                Fail(L"cannot create the DLL in %TEMP%", GetLastError());
        }

        DWORD Written = 0;
        const BOOL Wrote = WriteFile(File, kDllBytes, kDllSize, &Written, nullptr);
        CloseHandle(File);
        if (!Wrote || Written != kDllSize)
            Fail(L"WriteFile of the embedded DLL failed", GetLastError());

        wcsncpy_s(OutPath, OutPathCount, Candidate, _TRUNCATE);
    }

    void Inject(DWORD Pid, const wchar_t* DllPath)
    {
        HANDLE Process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                     FALSE, Pid);
        if (!Process)
        {
            EnableDebugPrivilege();
            Process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                                  PROCESS_VM_WRITE | PROCESS_VM_READ,
                                  FALSE, Pid);
            if (!Process)
                Fail(L"OpenProcess failed", GetLastError());
        }

        const SIZE_T PathBytes = (wcslen(DllPath) + 1) * sizeof(wchar_t);

        void* Remote = VirtualAllocEx(Process, nullptr, PathBytes,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!Remote)
            Fail(L"VirtualAllocEx failed", GetLastError());

        SIZE_T Done = 0;
        if (!WriteProcessMemory(Process, Remote, DllPath, PathBytes, &Done) || Done != PathBytes)
            Fail(L"WriteProcessMemory failed", GetLastError());

        // LoadLibraryW is at the same address in every process this boot.
        auto LoadLibraryAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        if (!LoadLibraryAddr)
            Fail(L"GetProcAddress(LoadLibraryW) failed", GetLastError());

        HANDLE Thread = CreateRemoteThread(Process, nullptr, 0, LoadLibraryAddr,
                                           Remote, 0, nullptr);
        if (!Thread)
            Fail(L"CreateRemoteThread failed", GetLastError());

        const DWORD Wait = WaitForSingleObject(Thread, 15000);
        CloseHandle(Thread);
        VirtualFreeEx(Process, Remote, 0, MEM_RELEASE);
        CloseHandle(Process);

        if (Wait != WAIT_OBJECT_0)
            Fail(L"remote LoadLibrary did not finish in 15 s");

        if (!HasModule(Pid, kDllName, /*bRetry=*/true))
            Fail(L"LoadLibrary finished but RideFlight.dll is not in the target "
                 L"(remote load returned NULL - dependency missing?)");

        DeleteFileW(DllPath);   // best-effort: the loader keeps the file locked
    }
}

int wmain()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    // %hs = narrow string in a wide format (MSVC extension).
    wprintf(L"RideFlight launcher (built %hs %hs, dll %u bytes)\n",
            __DATE__, __TIME__, static_cast<unsigned>(kDllSize));

    DWORD Pid = FindProcessId(kProcessName);
    if (Pid)
    {
        wprintf(L"[1/4] game running, pid=%lu\n", Pid);
    }
    else
    {
        wprintf(L"[1/4] game not running - start it now, waiting ...\n");
        for (;;)
        {
            Sleep(500);
            Pid = FindProcessId(kProcessName);
            if (Pid)
                break;
        }
        wprintf(L"[ OK ] game detected, pid=%lu\n", Pid);
    }

    if (HasModule(Pid, kDllName))
    {
        Ok(L"RideFlight.dll already loaded - nothing to do");
        wprintf(L"Press Enter to exit...\n");
        (void)getchar();
        return 0;
    }
    wprintf(L"[2/4] not injected yet\n");

    if (!WaitForMainWindow(Pid, 120000))
        Fail(L"no visible game window within 120 s - is the game on a login/splash screen?");
    Ok(L"game window is up");

    wchar_t DllPath[MAX_PATH] = {};
    ExtractDll(DllPath, MAX_PATH);
    wprintf(L"[3/4] extracted: %s\n", DllPath);

    Inject(Pid, DllPath);
    wprintf(L"[4/4] injected - hold Shift in-game to fly\n");

    Sleep(1500); // brief success flash before the window closes
    return 0;
}
