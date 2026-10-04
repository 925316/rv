#pragma once

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <type_traits>

// One line format -> timestamp -> stdout/wcout -> OutputDebugString.
namespace rv::log
{
	inline char g_last_line[600] = "(no output yet)";

	namespace detail
	{
		template <typename C>
		inline void emit(const C* buf)
		{
			SYSTEMTIME st{};
			GetLocalTime(&st);
			if constexpr (std::is_same_v<C, char>)
			{
				char ts[32];
				sprintf_s(ts, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
				std::cout << ts << buf << '\n';
				std::cout.flush();
				OutputDebugStringA(buf);
				strncpy_s(g_last_line, buf, _TRUNCATE);
				for (int i = (int)strlen(g_last_line) - 1; i >= 0 && (g_last_line[i] == '\n' || g_last_line[i] == '\r'); --i)
					g_last_line[i] = '\0';
			}
			else
			{
				wchar_t ts[32];
				swprintf_s(ts, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
				std::wcout << ts << buf << L'\n';
				std::wcout.flush();
				OutputDebugStringW(buf);
			}
		}
	}

	// Wrapping the format in a template parameter is what makes this work for both
	// char and wchar_t call sites, but it also hides the literal from clang's
	// format checker, which then flags every line as -Wformat-security. The
	// format strings are all literals at the call site; only the forwarding
	// hides that from the check.
#if defined(__clang__)
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wformat-security"
#endif
	template <typename C, typename... Args>
	inline void debug(const C* fmt, Args&&... args)
	{
		if constexpr (std::is_same_v<C, char>)
		{
			char buf[1024];
			snprintf(buf, sizeof(buf) - 1, fmt, std::forward<Args>(args)...);
			detail::emit(buf);
		}
		else
		{
			wchar_t buf[1024];
			swprintf_s(buf, _countof(buf) - 1, fmt, std::forward<Args>(args)...);
			detail::emit(buf);
		}
	}
#if defined(__clang__)
	#pragma clang diagnostic pop
#endif

	// One-shot variant for lines that would otherwise repeat every tick.
	// `reported` is the caller's own latch, so each message site reports at
	// most once without needing a registry or a log level.
	template <typename... Args>
	inline void debug_once(bool& reported, const char* fmt, Args&&... args)
	{
		if (reported)
			return;
		reported = true;
		debug(fmt, std::forward<Args>(args)...);
	}
}
