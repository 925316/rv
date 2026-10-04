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
		bool    m_mask[256] = {};   // true = byte must match, false = wildcard
		size_t  m_size = 0;
		bool    m_valid = false;
	};

	struct known_signature
	{
		const char* m_name;
		const char* m_text;
		uint32_t    m_rva;           // expected offset on the source build;
		// mismatch = game updated, table wants a refresh
	};

	// Verified unique on the target build (5.6.0-20702 rel-1.3).
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
		out.m_size = 0;
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
				out.m_mask[out.m_size] = false;
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
			out.m_mask[out.m_size] = true;
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

		const uint8_t  want = pat.m_bytes[anchor];
		const size_t   max_start = size - pat.m_size;   // inclusive
		size_t         start = 0;

		while (start <= max_start)
		{
			const size_t hay_start = start + anchor;
			const size_t hay_end = max_start + anchor;   // inclusive
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

	// fn(va, len, chars) per section, len clamped to SizeOfImage; fn may return
	// false to stop early.
	template <typename Fn>
	inline bool for_each_section(Fn&& fn)
	{
		const uint8_t* base = module_base();
		const size_t   img_size = module_size(base);
		if (!base || !img_size)
			return false;

		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
		const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			const uint32_t va = sec[i].VirtualAddress;
			if ((size_t)va >= img_size)
				continue;
			size_t len = sec[i].Misc.VirtualSize;
			if ((size_t)va + len > img_size)
				len = img_size - va;
			if (!fn(va, len, sec[i].Characteristics))
				return false;
		}
		return true;
	}

	// First match in executable sections. out_hits=null keeps early-out;
	// non-null continues the scan to count all matches (uniqueness check).
	inline const uint8_t* find_in_image(const pattern& pat, int* out_hits = nullptr)
	{
		if (out_hits)
			*out_hits = 0;

		const uint8_t* base = module_base();
		if (!base)
			return nullptr;

		const uint8_t* first = nullptr;
		int            hits = 0;
		for_each_section([&](uint32_t va, size_t len, uint32_t chars) -> bool
		{
			if (!(chars & IMAGE_SCN_MEM_EXECUTE))
				return true;

			// Count all matches per section: same-section repeat hits are ambiguous.
			const uint8_t* cur = base + va;
			size_t         left = len;
			while (left >= pat.m_size)
			{
				const uint8_t* hit = find_pattern(cur, left, pat);
				if (!hit)
					break;
				if (!first)
					first = hit;
				++hits;
				if (!out_hits)
					return false;   // caller did not ask to count: first hit wins
				const size_t step = (size_t)(hit - cur) + 1;
				cur += step;
				left -= step;
			}
			return true;
		});
		if (out_hits)
			*out_hits = hits;
		return first;
	}

	inline const void* find_in_image(const char* text)
	{
		pattern p;
		if (!parse(text, p))
			return nullptr;
		return find_in_image(p);
	}

	inline constexpr const char* APPEND_STRING_PATTERN = "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B F2 8B 19 48 8B F9 74 09 4C 8D 05 ?? ?? ?? ??";

	// Length of the legacy prefix run starting at p. A REX byte is NOT part of
	// this: in 64-bit mode it is the last prefix before the opcode, while
	// these may stack in any order before it.
	inline size_t legacy_prefix_len(const uint8_t* p, size_t avail)
	{
		size_t i = 0;
		while (i < avail)
		{
			const uint8_t b = p[i];
			const bool legacy = b == 0xF0 || b == 0xF2 || b == 0xF3   // lock, repne, rep
				|| b == 0x2E || b == 0x36 || b == 0x3E              // cs, ss, ds
				|| b == 0x26 || b == 0x64 || b == 0x65              // es, fs, gs
				|| b == 0x66 || b == 0x67;                          // operand, address size
			if (!legacy)
				break;
			++i;
		}
		return i;
	}

	// Two-byte opcodes that carry a RIP-relative memory operand. Anything else
	// behind 0x0F is skipped rather than guessed: reading the escape's second
	// byte as a modrm both invents references (0F 05 is syscall, whose 0x05
	// looks exactly like [rip+disp32]) and misses the real ones.
	inline bool rip_relative_two_byte_opcode(uint8_t opc2)
	{
		switch (opc2)
		{
		case 0x10:   // movups xmm, [rip+disp32]
		case 0x11:   // movups [rip+disp32], xmm
		case 0x28:   // movaps xmm, [rip+disp32]
		case 0x29:   // movaps [rip+disp32], xmm
		case 0x6F:   // movdqa xmm, [rip+disp32]
		case 0x7F:   // movdqa [rip+disp32], xmm
			return true;
		default:
			return false;
		}
	}

	// Visits RIP-relative memory operands (`opcode modrm disp32`, modrm mod=00
	// r/m=101, optional legacy prefixes, optional REX). Only targets in
	// writable sections reach visit; read-only refs still count in the return
	// total. target = image RVA of the displacement target.
	using rip_ref_visitor = void (*)(void* ctx, uint32_t target_rva, uint8_t opcode);

	inline uint64_t enumerate_rip_refs(rip_ref_visitor visit, void* ctx)
	{
		const uint8_t* base = module_base();
		const size_t   img_size = module_size(base);
		if (!base || !img_size || !visit)
			return 0;

		// Writable spans: a mutable global slot can only live in a section
		// the process writes to at runtime.
		struct region
		{
			uint32_t m_beg;
			uint32_t m_end;
		};
		region   writable[64];
		unsigned writable_n = 0;
		for_each_section([&](uint32_t va, size_t len, uint32_t chars) -> bool
		{
			if ((chars & IMAGE_SCN_MEM_WRITE) && writable_n < 64)
				writable[writable_n++] = { va, va + (uint32_t)len };
			return true;
		});

		const auto is_writable = [&](uint32_t t) -> bool
		{
			for (unsigned i = 0; i < writable_n; ++i)
				if (t >= writable[i].m_beg && t < writable[i].m_end)
					return true;
			return false;
		};

		uint64_t hits = 0;
		for_each_section([&](uint32_t va, size_t len, uint32_t chars) -> bool
		{
			if (!(chars & IMAGE_SCN_MEM_EXECUTE))
				return true;

			const uint8_t* sec = base + va;
			size_t         off = 0;
			while (off < len)
			{
				const size_t   avail = len - off;
				const uint8_t* cur = sec + off;

				const size_t prefix = legacy_prefix_len(cur, avail);
				if (prefix >= avail)
					break;

				size_t op = prefix;
				if ((cur[op] & 0xF0) == 0x40)   // REX
					++op;
				if (op >= avail)
					break;

				int32_t disp = 0;
				size_t instr_len = 0;
				if (cur[op] == 0x0F)
				{
					// modrm sits at op+2; the whole form is 7 bytes from cur.
					if (op + 7 > avail)
						break;
					if (!rip_relative_two_byte_opcode(cur[op + 1]) ||
						(cur[op + 2] & 0xC7) != 0x05)
					{
						++off;
						continue;
					}
					memcpy(&disp, cur + op + 3, 4);
					instr_len = op + 7;
				}
				else
				{
					// opcode + modrm + disp32, so 6 bytes from cur.
					if (op + 6 > avail)
						break;
					if ((cur[op + 1] & 0xC7) != 0x05)
					{
						++off;
						continue;
					}
					memcpy(&disp, cur + op + 2, 4);
					instr_len = op + 6;
				}

				const int64_t target = (int64_t)(va + off + instr_len) + disp;
				if (target >= 0 && (uint64_t)target < (uint64_t)img_size)
				{
					++hits;
					if (is_writable((uint32_t)target))
						visit(ctx, (uint32_t)target, cur[op]);
				}

				// Resume after the instruction so one reference is counted
				// once, not again from each of its prefix bytes. A form with
				// a trailing immediate is under-measured here; that only
				// costs one redundant re-scan.
				off += instr_len;
			}
			return true;
		});
		return hits;
	}
}
