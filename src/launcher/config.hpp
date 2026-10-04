#pragma once

// Shared constants, CLI target resolution, and process exit tiers.
// Pure functions here are intentionally free of Win32 calls for testability.

namespace launcher
{
	inline constexpr wchar_t kDefaultTargetExe[] = L"Ride-Win64-Shipping.exe";
	inline constexpr wchar_t kDllName[] = L"output.dll";
	inline constexpr wchar_t kDllBaseName[] = L"output";

	inline constexpr unsigned long kPollIntervalMs = 500;
	inline constexpr unsigned long kWindowTimeoutMs = 120000;
	inline constexpr unsigned long kRemoteThreadWaitMs = 15000;
	inline constexpr int kModuleRetryCount = 10;
	inline constexpr unsigned long kModuleRetryDelayMs = 500;

	enum class ExitCode : int
	{
		Success = 0,
		Fatal = 1, // injection/extract failures, non-recoverable without a fix
		Usage = 2, // bad CLI usage (--help, empty target name)
		NotFound = 3 // retryable: game window never appeared, user can retry
	};

	inline const wchar_t* resolve_target_exe(int argc, wchar_t* argv[], const wchar_t* fallback = kDefaultTargetExe)
	{
		if (argc >= 2 && argv != nullptr && argv[1] != nullptr && argv[1][0] != L'\0')
			return argv[1];
		return fallback;
	}

	inline bool is_help_arg(const wchar_t* arg)
	{
		if (arg == nullptr || arg[0] == L'\0')
			return false;
		return arg[0] == L'?' || arg[0] == L'-' || arg[0] == L'/';
	}
}
