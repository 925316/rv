#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

/*
 * Byte-pattern scanner with IDA-style wildcards: whitespace-separated hex
 * bytes, '?' and '??' both mean one wildcard byte, so patterns copied from
 * IDA or CE work unchanged.
 * Anchor-accelerated (memchr on the first concrete byte); scans executable
 * sections of the main module only (.text) and skips the data.
 */

namespace signatures
{
	struct pattern
	{
		uint8_t m_bytes[256] = {};
		bool    m_mask[256]  = {};   // true = byte must match, false = wildcard
		size_t  m_size       = 0;
		bool    m_valid      = false;
	};

	struct known_signature
	{
		const char* m_name;
		const char* m_text;
		uint32_t    m_rva;           // expected offset on the source build;
		                              // mismatch = game updated, table wants a refresh
	};

	// Verified unique against Ride-Win64-Shipping.exe 5.6.0-20702 rel-1.3.
	inline constexpr known_signature TABLE[] =
	{
		{ "ProcessEvent",
		  "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00 48 8D 6C 24 30 48 89 9D 28 01 00 00",
		  0x1495860 },
		{ "AddTorqueInRadians",
		  "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 60 48 8B DA 48 8B F1 E8 A6 69 BE FD 48 83 7B 20 00",
		  0x38A2130 },
		{ "GetPhysicsAngularVelRad",
		  "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 33 FF 49 8B F0 48 89 7C 24 58 48 8B DA 48 8B E9 E8 27 FA BB FD",
		  0x38A37D0 },
		{ "GetPhysicsLinearVelocity",
		  "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 33 FF 49 8B F0 48 89 7C 24 58 48 8B DA 48 8B E9 E8 77 F9 BB FD",
		  0x38A3880 },
		{ "SetPhysicsAngularVelRad",
		  "48 89 5C 24 18 56 57 41 56 48 83 EC 60 48 8B DA 4C 8B F1 E8 48 10 BE FD 48 83 7B 20 00",
		  0x38A7A90 },
		{ "SetPhysicsLinearVelocity",
		  "48 89 5C 24 18 56 57 41 56 48 83 EC 60 48 8B DA 4C 8B F1 E8 08 0F BE FD 48 83 7B 20 00",
		  0x38A7BD0 },
		{ "WakeRigidBody",
		  "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 33 FF 48 8B DA 48 89 7C 24 30 48 8B F1 E8 EF A5 BB FD",
		  0x38A8C10 },
		{ "K2_GetActorRotation",
		  "4C 8B DC 57 48 81 EC 90 00 00 00 48 8B 42 20 45 33 C9 48 85 C0 49 8B F8 41 0F 95 C1 4C 03 C8 4C 89 4A 20",
		  0x38E4C00 },
		{ "GetControlRotation",
		  "40 53 48 83 EC 40 48 8B 42 20 45 33 C9 48 85 C0 49 8B D8 41 0F 95 C1 4C 03 C8 4C 89 4A 20 48 8D 54 24 20 48 8B 01 FF 90 90 07 00 00 0F 10 00 0F 11 03",
		  0x3C4CD20 },
	};

	// Parse "48 89 ?5 ??" style text; false on anything that is not two hex
	// digits or a question mark.
	inline bool parse(const char* text, pattern& out)
	{
		out.m_size  = 0;
		out.m_valid = false;
		if (!text)
			return false;

		const char* p = text;
		while (*p)
		{
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
				++p;
			if (!*p)
				break;

			const char* token_start = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
				++p;
			const size_t token_len = (size_t)(p - token_start);

			if (out.m_size >= 256)
				return false;

			const bool wildcard = (token_len == 1 && token_start[0] == '?') ||
			                      (token_len == 2 && token_start[0] == '?' && token_start[1] == '?');
			if (wildcard)
			{
				out.m_bytes[out.m_size] = 0;
				out.m_mask[out.m_size]  = false;
				++out.m_size;
				continue;
			}

			if (token_len != 2)
				return false;

			auto nibble = [](char C) -> int
			{
				if (C >= '0' && C <= '9') return C - '0';
				if (C >= 'a' && C <= 'f') return C - 'a' + 10;
				if (C >= 'A' && C <= 'F') return C - 'A' + 10;
				return -1;
			};
			const int hi = nibble(token_start[0]);
			const int lo = nibble(token_start[1]);
			if (hi < 0 || lo < 0)
				return false;

			out.m_bytes[out.m_size] = (uint8_t)((hi << 4) | lo);
			out.m_mask[out.m_size]  = true;
			++out.m_size;
		}

		out.m_valid = (out.m_size > 0);
		return out.m_valid;
	}

	// First match of pat in [Data, Data+Size), or nullptr. Needs at least one
	// concrete byte - an all-wildcard pattern is always a caller bug.
	inline const uint8_t* find_pattern(const uint8_t* data, size_t size, const pattern& pat)
	{
		if (!pat.m_valid || !data || pat.m_size == 0 || size < pat.m_size)
			return nullptr;

		size_t anchor = 0;
		while (anchor < pat.m_size && !pat.m_mask[anchor])
			++anchor;
		if (anchor == pat.m_size)
			return nullptr;

		const uint8_t  want      = pat.m_bytes[anchor];
		const size_t   max_start = size - pat.m_size;   // inclusive
		size_t         start     = 0;

		while (start <= max_start)
		{
			const size_t hay_start = start + anchor;
			const size_t hay_end   = max_start + anchor;   // inclusive
			if (hay_start > hay_end)
				return nullptr;

			const void* hit = memchr(data + hay_start, want, hay_end - hay_start + 1);
			if (!hit)
				return nullptr;

			const size_t candidate = (size_t)((const uint8_t*)hit - data) - anchor;

			bool match = true;
			for (size_t i = 0; i < pat.m_size; ++i)
			{
				if (pat.m_mask[i] && data[candidate + i] != pat.m_bytes[i])
				{
					match = false;
					break;
				}
			}
			if (match)
				return data + candidate;

			start = candidate + 1;
		}
		return nullptr;
	}

	// Scan every executable section of the main module (the game EXE).
	inline const uint8_t* find_in_image(const pattern& pat)
	{
		const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
		if (!base)
			return nullptr;

		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return nullptr;

		const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return nullptr;

		const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
				continue;

			const uint8_t* begin = base + sec[i].VirtualAddress;
			const size_t   len   = sec[i].Misc.VirtualSize;
			if (const uint8_t* hit = find_pattern(begin, len, pat))
				return hit;
		}
		return nullptr;
	}

	inline const void* find_in_image(const char* text)
	{
		pattern p;
		if (!parse(text, p))
			return nullptr;
		return find_in_image(p);
	}

	inline constexpr const char* APPEND_STRING_PATTERN = "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B F2 8B 19 48 8B F9 74 09 4C 8D 05 ?? ?? ?? ??";

	// Mapped base of the main module image.
	inline const uint8_t* module_base()
	{
		return (const uint8_t*)GetModuleHandleW(nullptr);
	}

	// SizeOfImage from the PE headers; 0 when parsing fails.
	inline size_t module_size(const uint8_t* base)
	{
		if (!base)
			return 0;
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return 0;
		const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return 0;
		return nt->OptionalHeader.SizeOfImage;
	}

	// Visits every RIP-relative memory operand in the executable sections:
	// the 2-byte encoding form `opcode modrm disp32` with modrm mod=00 r/m=101
	// (optionally behind a REX-byte). target is the image-relative offset the
	// disp32 resolves to, opcode the byte at the operand start.
	using rip_ref_visitor = void (*)(void* ctx, uint32_t target_rva, uint8_t opcode);

	inline uint64_t enumerate_rip_refs(rip_ref_visitor visit, void* ctx)
	{
		const uint8_t* base = module_base();
		if (!base || !visit)
			return 0;

		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return 0;
		const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return 0;
		const size_t img_size = nt->OptionalHeader.SizeOfImage;

		uint64_t hits = 0;
		const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
				continue;

			const uint32_t va  = sec[i].VirtualAddress;
			size_t         len = sec[i].Misc.VirtualSize;
			if ((size_t)va + len > img_size)
				len = img_size - va;

			for (size_t off = 0; off < len; ++off)
			{
				size_t op = off;
				if ((base[va + off] & 0xF0) == 0x40)
					++op;
				if (va + op + 6 > va + len)
					continue;
				if ((base[va + op + 1] & 0xC7) != 0x05)
					continue;

				int32_t disp;
				memcpy(&disp, base + va + op + 2, 4);
				int64_t target = (int64_t)va + (int64_t)op + 6 + disp;
				if (target < 0 || (uint64_t)target >= (uint64_t)img_size)
					continue;

				visit(ctx, (uint32_t)target, base[va + op]);
				++hits;
			}
		}
		return hits;
	}
}
