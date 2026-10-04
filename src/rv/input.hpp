#pragma once

#include <Windows.h>

#include "state.hpp"

/*
 * Keyboard polling. Game-thread only, and only from the tick path: the keys
 * we read drive body writes, so a read from any other thread would race the
 * engine.
 */

namespace rv
{
	namespace helper
	{
		inline bool is_key_down(int virtual_key)
		{
			return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
		}

		inline double get_axis(int positive_key, int negative_key)
		{
			return (is_key_down(positive_key) ? 1.0 : 0.0) - (is_key_down(negative_key) ? 1.0 : 0.0);
		}
	}
}