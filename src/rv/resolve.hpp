#pragma once

#include <cstring>

#include "log.hpp"
#include "offsets.hpp"
#include "signatures.hpp"
#include "unreal.hpp"

// Startup resolution: signature first, pinned fallback second.
// A decoded target is adopted only after it passes validation.
namespace rv
{
	namespace detail
	{
		using unreal::read_ptr;
		using unreal::readable;

		inline bool plausible_ptr(int64_t p)
		{
			return p > 0x10000000 && (p & 7) == 0 && p < 0x00007FFF00000000;
		}

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

		inline bool valid_objects_at(const uint8_t* img, uint32_t rva, uint32_t img_size)
		{
			return valid_obj_array(img, rva, img_size) &&
				objects_chunk_ok(img, rva, img_size);
		}

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

			// TArray: { Data +0x0, Num +0x8, Max +0xC }.
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

		// validated = live world; not_ready = null/incomplete chain (early
		// inject, tick re-acquires); mismatch = garbage slot, RVA moved.
		enum class world_state { kValidated, kNotReady, kMismatch };

		inline world_state check_world_at(const uint8_t* img, uint32_t rva, uint32_t img_size)
		{
			if ((uint64_t)rva + 8 > img_size)
				return world_state::kMismatch;
			int64_t slot = 0;
			if (!read_ptr(img + rva, slot))
				return world_state::kMismatch;
			if (slot == 0)
				return world_state::kNotReady;
			if (!plausible_ptr(slot))
				return world_state::kMismatch;
			bool strict = false;
			world_chain(slot, img, img_size, strict);
			return strict ? world_state::kValidated : world_state::kNotReady;
		}
	} // namespace detail

	inline void resolve_globals()
	{
		unreal::g_objects_rva = unreal::G_OBJECTS_RVA;
		unreal::g_world_rva = unreal::G_WORLD_RVA;
		unreal::g_append_string_rva = unreal::APPEND_STRING_RVA;

		const uint8_t* img = signatures::module_base();
		if (!img)
		{
			log::debug("[sig] no image, fallback");
			return;
		}
		const uint32_t img_size = (uint32_t)signatures::module_size(img);
		if (!img_size)
		{
			log::debug("[sig] bad headers, fallback");
			return;
		}

		int via_sig = 0;

		// GObjects.
		{
			uint32_t sig_rva = 0;
			if (signatures::find_global_ref(signatures::GOBJECTS_REF_PATTERN,
				signatures::GOBJECTS_REF_DISP, signatures::GOBJECTS_REF_LEN,
				img_size, sig_rva) &&
				detail::valid_objects_at(img, sig_rva, img_size))
			{
				unreal::g_objects_rva = sig_rva;
				++via_sig;
			}
			else
				unreal::g_objects_rva = unreal::G_OBJECTS_RVA;
			const bool ok = detail::valid_objects_at(img, (uint32_t)unreal::g_objects_rva, img_size);
			log::debug("[sig] GObjects 0x%X %s%s", (uint32_t)unreal::g_objects_rva,
				unreal::g_objects_rva != unreal::G_OBJECTS_RVA ? "sig" : "fallback",
				ok ? "" : " BAD");
		}

		// GWorld.
		detail::world_state world = detail::world_state::kNotReady;
		{
			uint32_t sig_rva = 0;
			const bool sig_hit = signatures::find_global_ref(signatures::GWORLD_REF_PATTERN,
				signatures::GWORLD_REF_DISP, signatures::GWORLD_REF_LEN,
				img_size, sig_rva);
			detail::world_state sig_state = detail::world_state::kMismatch;
			if (sig_hit)
				sig_state = detail::check_world_at(img, sig_rva, img_size);
			if (sig_hit && sig_state != detail::world_state::kMismatch)
			{
				unreal::g_world_rva = sig_rva;
				world = sig_state;
				++via_sig;
			}
			else
			{
				unreal::g_world_rva = unreal::G_WORLD_RVA;
				world = detail::check_world_at(img, (uint32_t)unreal::g_world_rva, img_size);
			}
			const char* src = unreal::g_world_rva != unreal::G_WORLD_RVA ? "sig" : "fallback";
			const char* st = world == detail::world_state::kValidated ? "" :
				world == detail::world_state::kNotReady ? " pending" : " BAD";
			log::debug("[sig] GWorld 0x%X %s%s", (uint32_t)unreal::g_world_rva, src, st);
		}

		// AppendString: unique hit wins.
		bool append_ok = false;
		{
			signatures::pattern ap;
			int                 hits = 0;
			const uint8_t*      hit = nullptr;
			if (signatures::parse(signatures::APPEND_STRING_PATTERN, ap))
				hit = signatures::find_in_image(ap, &hits);
			if (hit && hits == 1)
			{
				unreal::g_append_string_rva = (uintptr_t)(hit - img);
				append_ok = true;
				++via_sig;
			}
			else
				unreal::g_append_string_rva = unreal::APPEND_STRING_RVA;
			log::debug("[sig] AppendString 0x%X %s%s", (uint32_t)unreal::g_append_string_rva,
				unreal::g_append_string_rva != unreal::APPEND_STRING_RVA ? "sig" : "fallback",
				append_ok || unreal::g_append_string_rva == unreal::APPEND_STRING_RVA ? "" : " BAD");
		}

		const bool objs_ok = detail::valid_objects_at(img, (uint32_t)unreal::g_objects_rva, img_size);
		if (!objs_ok || world == detail::world_state::kMismatch || !append_ok)
			log::debug("[sig] BAD: fix signatures.hpp + unreal.hpp (%d sig)", via_sig);
		else
			log::debug("[sig] %s (%d sig)", world == detail::world_state::kNotReady ? "2/3 pending world" : "3/3",
				via_sig);
	}
}
