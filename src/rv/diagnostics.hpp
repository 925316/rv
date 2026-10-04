#pragma once

#include <Windows.h>
#include <cstdint>

#include "log.hpp"
#include "signatures.hpp"
#include "unreal.hpp"

/*
 * Startup self-check. This is the difference between "the cheat does nothing"
 * and a one-line reason on the console, which matters because every silent
 * failure mode here looks identical from the outside.
 */

namespace rv
{
	namespace helper
	{
		// Anchor every native function once at startup: NOT FOUND = game update
		// wants a table refresh; a moved RVA with a match = code slid, signature holds.
		inline void verify_signatures()
		{
			const uint8_t* image_base = (const uint8_t*)GetModuleHandleW(nullptr);
			int found = 0;
			constexpr int total = (int)(sizeof(signatures::TABLE) / sizeof(signatures::TABLE[0]));

			for (const signatures::known_signature& sig : signatures::TABLE)
			{
				signatures::pattern p;
				if (!signatures::parse(sig.m_text, p))
				{
					log::debug("[sig] %-25s parse error", sig.m_name);
					continue;
				}

				const uint8_t* hit = signatures::find_in_image(p);
				if (!hit)
				{
					log::debug("[sig] %-25s NOT FOUND - game update?", sig.m_name);
					continue;
				}

				++found;
				const uint32_t rva = (uint32_t)(hit - image_base);
				if (rva == sig.m_rva)
					log::debug("[sig] %-25s ok rva 0x%X", sig.m_name, rva);
				else
					log::debug("[sig] %-25s moved rva 0x%X (table 0x%X)", sig.m_name, rva, sig.m_rva);
			}

			log::debug("[sig] %d/%d signatures resolved", found, total);
		}
	}
}