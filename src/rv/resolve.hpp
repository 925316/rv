#pragma once

#include <cstring>
#include <unordered_map>
#include <vector>

#include "log.hpp"
#include "offsets.hpp"
#include "signatures.hpp"
#include "unreal.hpp"

/*
 * Startup resolution of the three fallback RVAs; any failure keeps the
 * fallback and logs. Every layout probed here comes from offsets.hpp - the
 * same constants unreal.hpp pins its structs with - so the validator can
 * never disagree with the code it validates.
 */

namespace rv
{
	namespace detail
	{
		struct ref_stat
		{
			uint32_t m_refs = 0;
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

		// Memory-safety primitives live in unreal.hpp next to everything else
		// that dereferences engine memory; re-exported so the walks below read
		// as plain calls.
		using unreal::read_ptr;
		using unreal::readable;

		inline bool plausible_ptr(int64_t p)
		{
			return p > 0x10000000 && (p & 7) == 0 && p < 0x00007FFF00000000;
		}

		// TUObjectArray field consistency at a field-shifted candidate.
		// memcpy instead of a reinterpret_cast load: candidate offsets are
		// arbitrary byte positions in the image, so the fields are not
		// guaranteed to be aligned for a typed dereference.
		inline bool valid_obj_array(const uint8_t* img, uint32_t b, uint32_t img_size)
		{
			if ((uint64_t)b + offsets::OBJARRAY_SIZE > img_size)
				return false;

			int64_t tbl = 0;
			int32_t max_e = 0, num_e = 0, max_c = 0, num_c = 0;
			memcpy(&tbl, img + b + offsets::OBJARRAY_OBJECTS, sizeof(tbl));
			memcpy(&max_e, img + b + offsets::OBJARRAY_MAX_ELEMENTS, sizeof(max_e));
			memcpy(&num_e, img + b + offsets::OBJARRAY_NUM_ELEMENTS, sizeof(num_e));
			memcpy(&max_c, img + b + offsets::OBJARRAY_MAX_CHUNKS, sizeof(max_c));
			memcpy(&num_c, img + b + offsets::OBJARRAY_NUM_CHUNKS, sizeof(num_c));

			constexpr int32_t chunk_cap = unreal::ELEMENTS_PER_CHUNK;
			return tbl > 0x10000000 && (tbl & 7) == 0
				&& max_e > 0 && num_e > 0 && num_e <= max_e
				&& max_c > 0 && max_c <= chunk_cap && max_e <= (int64_t)max_c * chunk_cap
				&& num_c > 0 && num_c <= max_c && num_e <= (int64_t)num_c * chunk_cap;
		}

		// First chunk pointer of the array's chunk table must be plausible.
		inline bool objects_chunk_ok(const uint8_t* img, uint32_t b, uint32_t img_size)
		{
			if ((uint64_t)b + 8 > img_size)
				return false;
			int64_t tbl = *reinterpret_cast<const int64_t*>(img + b);
			int64_t c0 = 0;
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
			if (!read_ptr((const uint8_t*)world + offsets::UWORLD_PERSISTENT_LEVEL, level) ||
				!read_ptr((const uint8_t*)world + offsets::UWORLD_OWNING_GAME_INSTANCE, gi))
				return;
			if (!plausible_ptr(level) || !plausible_ptr(gi))
				return;

			// TArray layout: { Data +0x0, Num +0x8, Max +0xC }.
			int64_t data = 0;
			int32_t num = 0, max = 0;
			if (!read_ptr((const uint8_t*)gi + offsets::GAMEINSTANCE_LOCAL_PLAYERS_DATA, data) ||
				!read_ptr((const uint8_t*)gi + offsets::GAMEINSTANCE_LOCAL_PLAYERS_NUM, num) ||
				!read_ptr((const uint8_t*)gi + offsets::GAMEINSTANCE_LOCAL_PLAYERS_MAX, max))
				return;
			if (!plausible_ptr(data) || num < 1 || num > 8 || max < num || max > 16)
				return;

			int64_t lp0 = 0;
			if (!read_ptr((const void*)data, lp0))
				return;
			if (!plausible_ptr(lp0))
				return;

			int64_t pc = 0;
			if (!read_ptr((const uint8_t*)lp0 + offsets::LOCALPLAYER_PLAYER_CONTROLLER, pc))
				return;
			strict = plausible_ptr(pc);
		}
	} // namespace detail

		// Results into unreal::g_*_rva; log mirrors the debug_line tag.
	inline void resolve_globals()
	{
		const uint8_t* img = signatures::module_base();
		if (!img)
		{
			log::debug("[sig] resolve: no module image, keeping build constants");
			return;
		}
		const uint32_t img_size = (uint32_t)signatures::module_size(img);
		if (!img_size)
		{
			log::debug("[sig] resolve: bad PE headers, keeping build constants");
			return;
		}

		// Fast path: the pinned fallbacks still validate against the live
		// image -> skip the census entirely. Only fall through to the full
		// census when any of the three no longer matches (game updated,
		// offsets drifted).
		{
			const bool objs_ok =
				detail::valid_obj_array(img, unreal::G_OBJECTS_RVA, img_size) &&
				detail::objects_chunk_ok(img, unreal::G_OBJECTS_RVA, img_size);

			bool world_ok = false;
			if ((uint64_t)unreal::G_WORLD_RVA + 8 <= img_size)
			{
				int64_t slot = 0;
				if (detail::read_ptr(img + unreal::G_WORLD_RVA, slot) &&
					detail::plausible_ptr(slot))
				{
					bool strict = false;
					detail::world_chain(slot, img, img_size, strict);
					world_ok = strict;
				}
			}

			signatures::pattern ap;
			int                 hits = 0;
			const bool append_ok =
				signatures::parse(signatures::APPEND_STRING_PATTERN, ap) &&
				signatures::find_in_image(ap, &hits) && hits == 1;

			if (objs_ok && world_ok && append_ok)
			{
				log::debug("[sig] fallbacks validated, census skipped");
				log::debug("[sig] AppendString -> rva 0x%X",
					(uint32_t)unreal::APPEND_STRING_RVA);
				log::debug("[sig] GObjects -> rva 0x%X (fallback)",
					(uint32_t)unreal::G_OBJECTS_RVA);
				log::debug("[sig] GWorld -> rva 0x%X (fallback)",
					(uint32_t)unreal::G_WORLD_RVA);
				return;
			}
			log::debug("[sig] fallback validation failed (objs=%d world=%d append=%d), running census",
				objs_ok ? 1 : 0, world_ok ? 1 : 0, append_ok ? 1 : 0);
		}

		// 1) AppendString: adopted only when the full-prologue pattern hits
		// exactly once; ambiguous or missing patterns keep the fallback.
		{
			signatures::pattern ap;
			const uint8_t* hit = nullptr;
			int                 hits = 0;
			if (signatures::parse(signatures::APPEND_STRING_PATTERN, ap))
				hit = signatures::find_in_image(ap, &hits);
			if (hit && hits == 1)
			{
				unreal::g_append_string_rva = (uintptr_t)(hit - img);
				log::debug("[sig] AppendString -> rva 0x%X", (uint32_t)unreal::g_append_string_rva);
			}
			else if (hits > 1)
			{
				log::debug("[sig] AppendString: %d hits (ambiguous) - keeping fallback 0x%X",
					hits, (uint32_t)unreal::APPEND_STRING_RVA);
			}
			else
			{
				log::debug("[sig] AppendString: pattern not found - keeping fallback 0x%X",
					(uint32_t)unreal::APPEND_STRING_RVA);
			}
		}

		// 2) Census of RIP-relative references into writable data.
		std::unordered_map<uint32_t, detail::ref_stat> census;
		census.reserve(1 << 16);
		const uint64_t hits = signatures::enumerate_rip_refs(detail::rip_ref_accumulate, &census);
		log::debug("[sig] rip-ref census: %llu hits, %zu writable targets",
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

			uint32_t best_b = 0;
			uint32_t best_refs = 0;
			for (const auto& kv : go_pass)
			{
				if (!detail::objects_chunk_ok(img, kv.first, img_size))
					continue;
				if (kv.second > best_refs)
				{
					best_refs = kv.second;
					best_b = kv.first;
				}
			}
			if (best_refs > 0)
			{
				unreal::g_objects_rva = best_b;
				log::debug("[sig] GObjects -> rva 0x%X (refs=%u)", best_b, best_refs);
			}
			else
			{
				log::debug("[sig] GObjects: no validated candidate - keeping fallback 0x%X",
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

			uint32_t chosen = 0;
			uint32_t chosen_refs = 0;
			uint32_t chosen_writes = 0;
			if (best_total > 0)
			{
				for (const auto& c : strict)
				{
					if (c.m_world != best_world || c.m_writes == 0)
						continue;
					if (!chosen || c.m_refs > chosen_refs)
					{
						chosen = c.m_rva;
						chosen_refs = c.m_refs;
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
							chosen = c.m_rva;
							chosen_refs = c.m_refs;
							chosen_writes = c.m_writes;
						}
					}
				}
			}

			if (chosen)
			{
				unreal::g_world_rva = chosen;
				log::debug("[sig] GWorld -> rva 0x%X (refs=%u writes=%u)",
					chosen, chosen_refs, chosen_writes);
			}
			else
			{
				log::debug("[sig] GWorld: no strict candidate - keeping fallback 0x%X",
					(uint32_t)unreal::G_WORLD_RVA);
			}
		}
	}
}
