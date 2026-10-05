#pragma once

#include <cstring>

#include "log.hpp"
#include "offsets.hpp"
#include "signatures.hpp"
#include "unreal.hpp"

/*
 * Startup validation of the three pinned fallback RVAs. The fallbacks in
 * unreal.hpp are the truth; this only checks they still match the live
 * image so a game update becomes a one-line log instead of a silent
 * break. Any failure keeps the fallback and logs - no census, no
 * re-discovery, nothing slow on the startup path.
 * Every layout probed here comes from offsets.hpp - the same constants
 * unreal.hpp pins its structs with - so the validator can never disagree
 * with the code it validates.
 */

namespace rv
{
	namespace detail
	{
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

	// Fallbacks stay as-is; log mirrors the debug_line tag.
	inline void resolve_globals()
	{
		// Pinned values are the truth - re-assert in case anything wrote g_*.
		unreal::g_objects_rva = unreal::G_OBJECTS_RVA;
		unreal::g_world_rva = unreal::G_WORLD_RVA;
		unreal::g_append_string_rva = unreal::APPEND_STRING_RVA;

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

		const bool objs_ok =
			detail::valid_obj_array(img, unreal::G_OBJECTS_RVA, img_size) &&
			detail::objects_chunk_ok(img, unreal::G_OBJECTS_RVA, img_size);

		// Three states, because "no live world yet" and "offsets drifted"
		// look identical from a failed chain walk. A null slot or an
		// incomplete chain on a plausible world is normal on an early
		// inject - the tick re-acquires the world every frame. Only a
		// non-null garbage slot means the RVA itself moved.
		enum class world_state { kValidated, kNotReady, kMismatch };
		auto check_world = [&]() -> world_state
		{
			if ((uint64_t)unreal::G_WORLD_RVA + 8 > img_size)
				return world_state::kMismatch;
			int64_t slot = 0;
			if (!detail::read_ptr(img + unreal::G_WORLD_RVA, slot))
				return world_state::kMismatch;
			if (slot == 0)
				return world_state::kNotReady;
			if (!detail::plausible_ptr(slot))
				return world_state::kMismatch;
			bool strict = false;
			detail::world_chain(slot, img, img_size, strict);
			return strict ? world_state::kValidated : world_state::kNotReady;
		};
		const world_state world = check_world();

		signatures::pattern ap;
		int                 hits = 0;
		const bool append_ok =
			signatures::parse(signatures::APPEND_STRING_PATTERN, ap) &&
			signatures::find_in_image(ap, &hits) && hits == 1;

		log::debug("[sig] GObjects -> rva 0x%X (fallback%s)",
			(uint32_t)unreal::G_OBJECTS_RVA, objs_ok ? ", validated" : " - VALIDATION FAILED, game update?");
		if (world == world_state::kValidated)
			log::debug("[sig] GWorld -> rva 0x%X (fallback, validated)",
				(uint32_t)unreal::G_WORLD_RVA);
		else if (world == world_state::kNotReady)
			log::debug("[sig] GWorld -> rva 0x%X (fallback, world not ready yet - tick will re-acquire)",
				(uint32_t)unreal::G_WORLD_RVA);
		else
			log::debug("[sig] GWorld -> rva 0x%X (fallback - VALIDATION FAILED, game update?)",
				(uint32_t)unreal::G_WORLD_RVA);
		if (append_ok)
			log::debug("[sig] AppendString -> rva 0x%X (validated, unique hit)",
				(uint32_t)unreal::APPEND_STRING_RVA);
		else
			log::debug("[sig] AppendString pattern: %d hits - VALIDATION FAILED, game update?", hits);

		const bool world_bad = (world == world_state::kMismatch);
		if (!objs_ok || world_bad || !append_ok)
			log::debug("[sig] fix the RVA in src/rv/unreal.hpp, keeping fallbacks");
		else if (world == world_state::kNotReady)
			log::debug("[sig] 2/3 globals validated, GWorld pending a live world");
		else
			log::debug("[sig] 3/3 globals validated");
	}
}
