#pragma once

#include <unordered_map>
#include <vector>

#include "signatures.hpp"
#include "unreal.hpp"

/*
 * Startup resolution of the three hand-pinned RVAs. Each probe mirrors the
 * live-memory walk that pinned the fallback constant; any failure simply
 * keeps the fallback and logs, never crashes.
 */

namespace rv
{
namespace detail
{
	struct ref_stat
	{
		uint32_t m_refs   = 0;
		uint32_t m_writes = 0;
	};

	inline void rip_ref_accumulate(void* ctx, uint32_t target, uint8_t opcode)
	{
		auto* map = (std::unordered_map<uint32_t, ref_stat>*)ctx;
		ref_stat& s = (*map)[target];
		++s.m_refs;
		// Memory-destination store opcodes => target slot is written.
		if (opcode == 0x89 || opcode == 0x88 || opcode == 0xC7 ||
		    opcode == 0xC6 || opcode == 0x87)
			++s.m_writes;
	}

	// Untrusted pointer check: refuse to touch unmapped or guarded memory so
	// a bad candidate degrades to "rejected", never a crash.
	inline bool readable(const void* addr, size_t len)
	{
		const uint8_t* p   = (const uint8_t*)addr;
		const uint8_t* end = p + len;
		while (p < end)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
				return false;
			if (mbi.State != MEM_COMMIT ||
			    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
				return false;
			const uint8_t* region = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
			p = region > p ? region : end;
		}
		return true;
	}

	template <typename T>
	inline bool read_ptr(const void* addr, T& out)
	{
		if (!readable(addr, sizeof(T)))
			return false;
		out = *reinterpret_cast<const T*>(addr);
		return true;
	}

	inline bool plausible_ptr(int64_t p)
	{
		return p > 0x10000000 && (p & 7) == 0 && p < 0x00007FFF00000000;
	}

	// TUObjectArray field consistency at a field-shifted candidate.
	inline bool valid_obj_array(const uint8_t* img, uint32_t b, uint32_t img_size)
	{
		if ((uint64_t)b + 0x20 > img_size)
			return false;
		int64_t tbl   = *reinterpret_cast<const int64_t*>(img + b);
		int32_t max_e = *reinterpret_cast<const int32_t*>(img + b + 0x10);
		int32_t num_e = *reinterpret_cast<const int32_t*>(img + b + 0x14);
		int32_t max_c = *reinterpret_cast<const int32_t*>(img + b + 0x18);
		int32_t num_c = *reinterpret_cast<const int32_t*>(img + b + 0x1C);
		return tbl > 0x10000000 && (tbl & 7) == 0
		      && max_e > 0 && num_e > 0 && num_e <= max_e
		      && max_c > 0 && max_c <= 0x10000 && max_e <= (int64_t)max_c * 0x10000
		      && num_c > 0 && num_c <= max_c && num_e <= (int64_t)num_c * 0x10000;
	}

	// First chunk pointer of the array's chunk table must be plausible.
	inline bool objects_chunk_ok(const uint8_t* img, uint32_t b, uint32_t img_size)
	{
		if ((uint64_t)b + 8 > img_size)
			return false;
		int64_t tbl = *reinterpret_cast<const int64_t*>(img + b);
		int64_t c0  = 0;
		if (!read_ptr((const void*)tbl, c0))
			return false;
		return plausible_ptr(c0);
	}

	// Walk UWorld -> PersistentLevel/GameInstance -> LocalPlayers -> PC.
	// strict is set only when the whole chain reaches a plausible controller.
	inline void world_chain(int64_t world, const uint8_t* base, uint32_t img_size, bool& strict)
	{
		strict = false;

		int64_t vt = 0;
		if (!read_ptr((const void*)world, vt))
			return;
		if (vt < (int64_t)base || vt >= (int64_t)base + img_size)
			return;

		int64_t level = 0, gi = 0;
		if (!read_ptr((const uint8_t*)world + 0x30, level) ||
		    !read_ptr((const uint8_t*)world + 0x228, gi))
			return;
		if (!plausible_ptr(level) || !plausible_ptr(gi))
			return;

		int64_t data = 0;
		int32_t num = 0, max = 0;
		if (!read_ptr((const uint8_t*)gi + 0x38, data) ||
		    !read_ptr((const uint8_t*)gi + 0x40, num) ||
		    !read_ptr((const uint8_t*)gi + 0x44, max))
			return;
		if (!plausible_ptr(data) || num < 1 || num > 8 || max < num || max > 16)
			return;

		int64_t lp0 = 0;
		if (!read_ptr((const void*)data, lp0))
			return;
		if (!plausible_ptr(lp0))
			return;

		int64_t pc = 0;
		if (!read_ptr((const uint8_t*)lp0 + 0x30, pc))
			return;
		strict = plausible_ptr(pc);
	}

} // namespace detail

	// log receives the same tagged lines debug_line prints; pass rv's printf
	// wrapper or a no-op. Results are written to unreal::g_*_rva; fallbacks kept on failure.
	inline void resolve_globals(void (*log)(const char* fmt, ...))
	{
		const uint8_t* img = signatures::module_base();
		if (!img)
		{
			log("[sig] resolve: no module image, keeping build constants\n");
			return;
		}
		const uint32_t img_size = (uint32_t)signatures::module_size(img);
		if (!img_size)
		{
			log("[sig] resolve: bad PE headers, keeping build constants\n");
			return;
		}

		// 1) AppendString: unique full-prologue hit in executable code.
		{
			signatures::pattern ap;
			const uint8_t* hit = nullptr;
			if (signatures::parse(signatures::APPEND_STRING_PATTERN, ap))
				hit = signatures::find_in_image(ap);
			if (hit)
			{
				unreal::g_append_string_rva = (uintptr_t)(hit - img);
				log("[sig] AppendString -> rva 0x%X\n", (uint32_t)unreal::g_append_string_rva);
			}
			else
			{
				log("[sig] AppendString: pattern not found - keeping fallback 0x%X\n",
				    (uint32_t)unreal::APPEND_STRING_RVA);
			}
		}

		// 2) Census of all RIP-relative memory operands.
		std::unordered_map<uint32_t, detail::ref_stat> census;
		const uint64_t hits = signatures::enumerate_rip_refs(detail::rip_ref_accumulate, &census);
		log("[sig] rip-ref census: %llu hits, %zu targets\n",
		    (unsigned long long)hits, census.size());

		// 3) GObjects: TUObjectArray layout + chunk deref, rank by refs.
		{
			static constexpr uint32_t shifts[] = { 0, 8, 0x10, 0x14, 0x18, 0x1C };
			std::unordered_map<uint32_t, uint32_t> go_pass;
			for (const auto& kv : census)
			{
				const uint32_t t = kv.first;
				for (uint32_t sh : shifts)
				{
					if (t < sh)
						continue;
					const uint32_t b = t - sh;
					if (!detail::valid_obj_array(img, b, img_size))
						continue;
					go_pass[b] += kv.second.m_refs;
					break;
				}
			}

			uint32_t best_b    = 0;
			uint32_t best_refs = 0;
			for (const auto& kv : go_pass)
			{
				if (!detail::objects_chunk_ok(img, kv.first, img_size))
					continue;
				if (kv.second > best_refs)
				{
					best_refs = kv.second;
					best_b    = kv.first;
				}
			}
			if (best_refs > 0)
			{
				unreal::g_objects_rva = best_b;
				log("[sig] GObjects -> rva 0x%X (refs=%u)\n", best_b, best_refs);
			}
			else
			{
				log("[sig] GObjects: no validated candidate - keeping fallback 0x%X\n",
				    (uint32_t)unreal::G_OBJECTS_RVA);
			}
		}

		// 4) GWorld: strict chain, world-value group by summed refs,
		// prefer write-through slots, then max refs.
		{
			struct cand
			{
				uint32_t m_rva;
				uint32_t m_refs;
				uint32_t m_writes;
				uint64_t m_world;
			};
			std::vector<cand> strict;
			for (const auto& kv : census)
			{
				const uint32_t t = kv.first;
				if ((uint64_t)t + 8 > img_size)
					continue;
				int64_t slot = *reinterpret_cast<const int64_t*>(img + t);
				if (!detail::plausible_ptr(slot))
					continue;
				bool is_strict = false;
				detail::world_chain(slot, img, img_size, is_strict);
				if (!is_strict)
					continue;
				strict.push_back({ t, kv.second.m_refs, kv.second.m_writes, (uint64_t)slot });
			}

			std::unordered_map<uint64_t, uint32_t> group_refs;
			for (const auto& c : strict)
				group_refs[c.m_world] += c.m_refs;

			uint64_t best_world = 0;
			uint32_t best_total = 0;
			for (const auto& kv : group_refs)
				if (kv.second > best_total)
				{
					best_total = kv.second;
					best_world = kv.first;
				}

			uint32_t chosen        = 0;
			uint32_t chosen_refs   = 0;
			uint32_t chosen_writes = 0;
			if (best_total > 0)
			{
				for (const auto& c : strict)
				{
					if (c.m_world != best_world || c.m_writes == 0)
						continue;
					if (!chosen || c.m_refs > chosen_refs)
					{
						chosen        = c.m_rva;
						chosen_refs   = c.m_refs;
						chosen_writes = c.m_writes;
					}
				}
				if (!chosen)
				{
					for (const auto& c : strict)
					{
						if (c.m_world != best_world)
							continue;
						if (!chosen || c.m_refs > chosen_refs)
						{
							chosen        = c.m_rva;
							chosen_refs   = c.m_refs;
							chosen_writes = c.m_writes;
						}
					}
				}
			}

			if (chosen)
			{
				unreal::g_world_rva = chosen;
				log("[sig] GWorld -> rva 0x%X (refs=%u writes=%u)\n",
				    chosen, chosen_refs, chosen_writes);
			}
			else
			{
				log("[sig] GWorld: no strict candidate - keeping fallback 0x%X\n",
				    (uint32_t)unreal::G_WORLD_RVA);
			}
		}
	}
}
