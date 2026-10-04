#pragma once

#include <cstddef>

/*
 * Central UE member offsets for the target game build (5.6.0-20702 rel-1.3).
 * Single source of truth: unreal.hpp layouts pin these via static_assert and
 * resolve.hpp world_chain() reads through them. Values are mechanical -
 * update them together when the game build changes.
 */

namespace offsets
{
	// UWorld (base UObject ends at 0x28).
	inline constexpr size_t UWORLD_PERSISTENT_LEVEL = 0x30;
	inline constexpr size_t UWORLD_OWNING_GAME_INSTANCE = 0x228;

	// UGameInstance::LocalPlayers is a TArray at 0x38; Data/Num/Max follow
	// the TArray layout { ptr +0x0, int32 +0x8, int32 +0xC }.
	inline constexpr size_t GAMEINSTANCE_LOCAL_PLAYERS = 0x38;
	inline constexpr size_t GAMEINSTANCE_LOCAL_PLAYERS_DATA = 0x38;
	inline constexpr size_t GAMEINSTANCE_LOCAL_PLAYERS_NUM = 0x40;
	inline constexpr size_t GAMEINSTANCE_LOCAL_PLAYERS_MAX = 0x44;

	// ULocalPlayer::PlayerController.
	inline constexpr size_t LOCALPLAYER_PLAYER_CONTROLLER = 0x30;

	// APlayerController::Pawn.
	inline constexpr size_t PLAYERCONTROLLER_PAWN = 0x2E8;

	// AVehicleBase (BP vehicle) fields at absolute offsets.
	inline constexpr size_t VEHICLE_MESH = 0x4C0;
	inline constexpr size_t VEHICLE_DYNAMIC_AIR_DRAG = 0x708;
	inline constexpr size_t VEHICLE_BASE_LINEAR_DRAG = 0x710;
	inline constexpr size_t VEHICLE_DEFAULT_LINEAR_DRAG = 0x718;
	inline constexpr size_t VEHICLE_SPRING_DOWNFORCE = 0xB38;
	inline constexpr size_t VEHICLE_BLOCK_DOWNWARD_FORCE_IN_AIR = 0xE28;

	// TUObjectArray image-layout fields (resolve.hpp validation).
	inline constexpr size_t OBJARRAY_OBJECTS = 0x00;
	inline constexpr size_t OBJARRAY_MAX_ELEMENTS = 0x10;
	inline constexpr size_t OBJARRAY_NUM_ELEMENTS = 0x14;
	inline constexpr size_t OBJARRAY_MAX_CHUNKS = 0x18;
	inline constexpr size_t OBJARRAY_NUM_CHUNKS = 0x1C;
	inline constexpr size_t OBJARRAY_SIZE = 0x20;
}
