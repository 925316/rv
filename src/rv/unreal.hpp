#pragma once

#include <Windows.h>
#include <cstdint>
#include <cwchar>
#include <string>

#include "offsets.hpp"

/*
 * Hand-maintained UE layer; no generated SDK. Member offsets live in
 * offsets.hpp, which every layout below and resolve.hpp both read, so a game
 * update is one edit. signatures.hpp reports drift against the target build
 * (5.6.0-20702 rel-1.3).
 *
 * clang and GCC reject offsetof() on these types with -Winvalid-offsetof: they
 * inherit across UObject/UField/UStruct and carry member functions, so the
 * standard no longer guarantees a layout. Verifying the padding of a mirror
 * whose layout we do not control is the one job offsetof() can still do here,
 * which is exactly what the static_asserts below are for. MSVC does not warn.
 */
#if defined(__clang__) || defined(__GNUC__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

namespace unreal
{
	// Compile-time fallbacks; rv::resolve_globals() overwrites g_* at startup.
	inline constexpr uintptr_t G_OBJECTS_RVA = 0x0A50E640;
	inline constexpr uintptr_t G_WORLD_RVA = 0x0A272A50;
	inline constexpr uintptr_t APPEND_STRING_RVA = 0x01256FE0;
	inline constexpr int       PROCESS_EVENT_IDX = 0x4C;

	// Runtime-resolved at startup before hook install; read-only afterwards.
	inline uintptr_t g_objects_rva = G_OBJECTS_RVA;
	inline uintptr_t g_world_rva = G_WORLD_RVA;
	inline uintptr_t g_append_string_rva = APPEND_STRING_RVA;

	inline void* image_base()
	{
		return GetModuleHandleW(nullptr);
	}

	// Untrusted pointer check: refuse to touch unmapped or guarded memory, so a
	// garbage engine slot degrades to "rejected" instead of killing the process
	// (the crash filter terminates). Every address the game hands us that we
	// dereference goes through this - including the hot path, not just the
	// one-off resolver.
	inline bool readable(const void* addr, size_t len)
	{
		const uint8_t* p = (const uint8_t*)addr;
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

	struct FVector
	{
		double X = 0.0, Y = 0.0, Z = 0.0;
	};

	struct FRotator
	{
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
	};

	struct FName
	{
		int32_t  Index = 0;
		uint32_t Number = 0;
	};

	struct FString
	{
		wchar_t* Data;
		int32_t  Num;
		int32_t  Max;
	};

	struct UFunction;

	struct UObject
	{
		void* VTable;    // 0x00
		int32_t  Flags;     // 0x08 EObjectFlags
		int32_t  Index;     // 0x0C InternalIndex
		UObject* Class;     // 0x10
		FName    Name;      // 0x18
		UObject* Outer;     // 0x20

		void process_event(UFunction* fn, void* parms) const
		{
			using PE_t = void (*)(const UObject*, UFunction*, void*);
			void** vtable = *reinterpret_cast<void***>(const_cast<UObject*>(this));
			PE_t pe = reinterpret_cast<PE_t>(vtable[PROCESS_EVENT_IDX]);
			pe(this, fn, parms);
		}
	};
	static_assert(sizeof(UObject) == 0x28, "UObject layout");

	struct UField : UObject
	{
		UField* Next;   // 0x28
	};
	static_assert(sizeof(UField) == 0x30, "UField layout");

	struct UStruct : UField
	{
		uint8_t  BaseChain[0x10];   // 0x30 FStructBaseChain (unused here)
		UStruct* SuperStruct;       // 0x40
		UField* Children;          // 0x48
		// ChildProperties 0x50, Size 0x58 - unused
	};
	static_assert(offsetof(UStruct, SuperStruct) == 0x40, "UStruct layout");
	static_assert(offsetof(UStruct, Children) == 0x48, "UStruct layout");

	struct UFunction : UStruct
	{
		uint8_t  Pad_50[0x60];       // 0x50 .. 0xB0
		uint32_t FunctionFlags;      // 0xB0 EFunctionFlags
		uint8_t  Pad_B4[0x24];       // 0xB4 .. 0xD8
		void* ExecFunction;       // 0xD8 native thunk / VM entry
	};
	static_assert(sizeof(UFunction) == 0xE0, "UFunction layout");

	struct FUObjectItem
	{
		UObject* Object;
		uint8_t  Pad[0x10];
	};
	static_assert(sizeof(FUObjectItem) == 0x18, "FUObjectItem layout");

	struct TUObjectArray
	{
		FUObjectItem** Objects;     // 0x00 chunk table
		uint8_t        Pad_08[0x8]; // 0x08
		int32_t        MaxElements; // 0x10
		int32_t        NumElements; // 0x14
		int32_t        MaxChunks;   // 0x18
		int32_t        NumChunks;   // 0x1C
	};
	static_assert(sizeof(TUObjectArray) == 0x20, "TUObjectArray layout");

	inline constexpr int32_t ELEMENTS_PER_CHUNK = 0x10000;

	inline TUObjectArray* g_objects()
	{
		return reinterpret_cast<TUObjectArray*>(
			static_cast<uint8_t*>(image_base()) + g_objects_rva);
	}

	// Chunk table entry: bounds-checked and guarded. The chunk pointer lives
	// in engine memory we did not allocate, so a wrong GObjects RVA must
	// not become a wild read - the table slot itself is readable()'d before
	// it is trusted.
	inline FUObjectItem* chunk_at(const TUObjectArray* arr, int32_t chunk_idx)
	{
		if (!readable(arr, sizeof(TUObjectArray)))
			return nullptr;
		TUObjectArray snapshot{};
		if (!read_ptr(arr, snapshot))
			return nullptr;
		if (!snapshot.Objects || chunk_idx < 0 || chunk_idx >= snapshot.NumChunks)
			return nullptr;
		// Address arithmetic only: no dereference of engine memory yet.
		if (!readable(&snapshot.Objects[chunk_idx], sizeof(FUObjectItem*)))
			return nullptr;
		FUObjectItem* chunk = nullptr;
		if (!read_ptr(&snapshot.Objects[chunk_idx], chunk))
			return nullptr;
		return chunk;
	}

	inline UObject* get_by_index(int32_t index)
	{
		const TUObjectArray* arr = g_objects();
		if (!readable(arr, sizeof(TUObjectArray)))
			return nullptr;
		TUObjectArray snapshot{};
		if (!read_ptr(arr, snapshot))
			return nullptr;
		if (index < 0 || index >= snapshot.NumElements)
			return nullptr;

		FUObjectItem* chunk = chunk_at(arr, index / ELEMENTS_PER_CHUNK);
		if (!chunk)
			return nullptr;

		if (!readable(&chunk[index % ELEMENTS_PER_CHUNK], sizeof(FUObjectItem)))
			return nullptr;
		FUObjectItem item{};
		if (!read_ptr(&chunk[index % ELEMENTS_PER_CHUNK], item))
			return nullptr;
		return item.Object;
	}

	// UE's own object-validity rule: the pointer must still be the object
	// registered at its own index in TUObjectArray. A destroyed object either
	// kept a stale index or was replaced in that slot, so both halves must
	// match. This is what makes writing to a "stale" pointer safe to attempt.
	// The obj and obj->Index reads are guarded here, so callers must NOT
	// readable() beforehand - pass the raw pointer straight in.
	inline bool is_valid(const UObject* obj)
	{
		if (!obj)
			return false;
		if (!readable(obj, sizeof(UObject)))
			return false;
		// &obj->Index is address arithmetic, not a dereference, so this is
		// safe even for a wild pointer; readable() then rejects it.
		if (!readable(&obj->Index, sizeof(obj->Index)))
			return false;
		int32_t index = 0;
		if (!read_ptr(&obj->Index, index))
			return false;
		return get_by_index(index) == obj;
	}

	// AppendString callee expects a caller-supplied buffer; never reallocate it.
	inline std::wstring name_to_string(const FName& name)
	{
		using Append_t = void (*)(const FName*, FString&);
		const Append_t append = reinterpret_cast<Append_t>(
			static_cast<uint8_t*>(image_base()) + g_append_string_rva);

		wchar_t buffer[1024] = {};
		FString str{ buffer, 0, 1024 };
		append(&name, str);

		// AppendString pads Str.Num with the trailing L'\0' - use wcslen.
		const wchar_t* data = str.Data ? str.Data : buffer;
		return std::wstring(data, data + wcslen(data));
	}

	inline bool name_equals(const FName& name, const wchar_t* literal)
	{
		return name_to_string(name) == literal;
	}

	inline bool is_a(const UObject* obj, const wchar_t* class_name)
	{
		if (!obj || !obj->Class)
			return false;

		for (UStruct* clss = reinterpret_cast<UStruct*>(obj->Class);
			clss; clss = clss->SuperStruct)
		{
			if (name_equals(clss->Name, class_name))
				return true;
		}
		return false;
	}

	// Walks SuperStruct for the declaring class, then Children for the field.
	inline UFunction* find_function(UObject* context, const wchar_t* class_name,
		const wchar_t* func_name)
	{
		if (!context || !context->Class)
			return nullptr;

		for (UStruct* clss = reinterpret_cast<UStruct*>(context->Class);
			clss; clss = clss->SuperStruct)
		{
			if (!name_equals(clss->Name, class_name))
				continue;

			for (UField* field = clss->Children; field; field = field->Next)
			{
				if (name_equals(field->Name, func_name))
					return reinterpret_cast<UFunction*>(field);
			}
		}
		return nullptr;
	}

	// Flat structs, padded to absolute offsets from offsets.hpp.
	struct UWorld : UObject
	{
		uint8_t  Pad_28[offsets::UWORLD_PERSISTENT_LEVEL - 0x28];
		UObject* PersistentLevel;
		uint8_t  Pad_38[offsets::UWORLD_OWNING_GAME_INSTANCE - offsets::UWORLD_PERSISTENT_LEVEL - sizeof(void*)];
		UObject* OwningGameInstance;
	};
	static_assert(offsetof(UWorld, PersistentLevel) == offsets::UWORLD_PERSISTENT_LEVEL, "UWorld layout");
	static_assert(offsetof(UWorld, OwningGameInstance) == offsets::UWORLD_OWNING_GAME_INSTANCE, "UWorld layout");

	struct TArray
	{
		void* Data;
		int32_t Num;
		int32_t Max;

		void* at(int32_t i) const
		{
			// Named like std::vector::at, so it has to check: the engine can
			// hand us Num > 0 with Data == null across a level transition, and
			// this is a raw index into Data.
			if (!Data || i < 0 || i >= Num)
				return nullptr;
			return static_cast<void**>(Data)[i];
		}
	};

	struct UGameInstance : UObject
	{
		uint8_t Pad_28[offsets::GAMEINSTANCE_LOCAL_PLAYERS - 0x28];
		TArray  LocalPlayers;
	};
	static_assert(offsetof(UGameInstance, LocalPlayers) == offsets::GAMEINSTANCE_LOCAL_PLAYERS, "UGameInstance layout");

	struct ULocalPlayer : UObject
	{
		uint8_t  Pad_28[offsets::LOCALPLAYER_PLAYER_CONTROLLER - 0x28];
		UObject* PlayerController;
	};
	static_assert(offsetof(ULocalPlayer, PlayerController) == offsets::LOCALPLAYER_PLAYER_CONTROLLER, "ULocalPlayer layout");

	struct APlayerController : UObject
	{
		uint8_t Pad_28[offsets::PLAYERCONTROLLER_PAWN - 0x28];
		UObject* Pawn;
	};
	static_assert(offsetof(APlayerController, Pawn) == offsets::PLAYERCONTROLLER_PAWN, "AController layout");

	struct APawn : UObject
	{
	};

	struct UPrimitiveComponent : UObject
	{
	};

	// BP vehicle: fields at absolute offsets.
	struct AVehicleBase : APawn
	{
		uint8_t  Pad_28[offsets::VEHICLE_MESH - 0x28];
		UObject* VehicleMesh;                 // UStaticMeshComponent*
		uint8_t  Pad_4C8[offsets::VEHICLE_DYNAMIC_AIR_DRAG - offsets::VEHICLE_MESH - sizeof(void*)];
		bool     DynamicAirDrag;
		uint8_t  Pad_709[offsets::VEHICLE_BASE_LINEAR_DRAG - offsets::VEHICLE_DYNAMIC_AIR_DRAG - sizeof(bool)];
		double   BaseLinearDrag;
		double   DefaultLinearDrag;
		uint8_t  Pad_720[offsets::VEHICLE_SPRING_DOWNFORCE - offsets::VEHICLE_DEFAULT_LINEAR_DRAG - sizeof(double)];
		double   SpringDownforce;
		uint8_t  Pad_B40[offsets::VEHICLE_BLOCK_DOWNWARD_FORCE_IN_AIR - offsets::VEHICLE_SPRING_DOWNFORCE - sizeof(double)];
		bool     BlockDownwardForceInAir;
	};
	static_assert(offsetof(AVehicleBase, VehicleMesh) == offsets::VEHICLE_MESH, "VehicleMesh layout");
	static_assert(offsetof(AVehicleBase, DynamicAirDrag) == offsets::VEHICLE_DYNAMIC_AIR_DRAG, "DynamicAirDrag layout");
	static_assert(offsetof(AVehicleBase, BaseLinearDrag) == offsets::VEHICLE_BASE_LINEAR_DRAG, "BaseLinearDrag layout");
	static_assert(offsetof(AVehicleBase, DefaultLinearDrag) == offsets::VEHICLE_DEFAULT_LINEAR_DRAG, "DefaultLinearDrag layout");
	static_assert(offsetof(AVehicleBase, SpringDownforce) == offsets::VEHICLE_SPRING_DOWNFORCE, "SpringDownforce layout");
	static_assert(offsetof(AVehicleBase, BlockDownwardForceInAir) == offsets::VEHICLE_BLOCK_DOWNWARD_FORCE_IN_AIR, "BlockDownwardForceInAir layout");

	inline UWorld* get_world()
	{
		return *reinterpret_cast<UWorld**>(
			static_cast<uint8_t*>(image_base()) + g_world_rva);
	}

	// Every UFunction is looked up once and cached in a function-local static.
	// A lookup that fails used to degrade into a silent zero-return or a
	// no-op depending on which wrapper it was, and the flight code then
	// saved that zero as "the state we found" - disengage would write a
	// value it never read back onto a live component. So every wrapper now
	// reports whether the call actually happened: false means the call was
	// skipped and the out value is untouched and meaningless.
	template <typename Parms>
	inline bool invoke(UObject* obj, UFunction*& cache, const wchar_t* class_name,
		const wchar_t* func_name, Parms& parms)
	{
		if (!obj)
			return false;
		if (!cache)
			cache = find_function(obj, class_name, func_name);
		if (!cache)
			return false;
		obj->process_event(cache, &parms);
		return true;
	}

	inline bool is_gravity_enabled(UObject* mesh, bool& out_enabled)
	{
		static UFunction* fn = nullptr;
		struct { bool ReturnValue; } p{};
		if (!invoke(mesh, fn, L"PrimitiveComponent", L"IsGravityEnabled", p))
			return false;
		out_enabled = p.ReturnValue;
		return true;
	}

	inline bool set_enable_gravity(UObject* mesh, bool enable)
	{
		static UFunction* fn = nullptr;
		struct { bool bGravityEnabled; } p{ enable };
		return invoke(mesh, fn, L"PrimitiveComponent", L"SetEnableGravity", p);
	}

	inline bool get_linear_damping(UObject* mesh, float& out_damping)
	{
		static UFunction* fn = nullptr;
		struct { float ReturnValue; } p{};
		if (!invoke(mesh, fn, L"PrimitiveComponent", L"GetLinearDamping", p))
			return false;
		out_damping = p.ReturnValue;
		return true;
	}

	inline bool set_linear_damping(UObject* mesh, float in_damping)
	{
		static UFunction* fn = nullptr;
		struct { float InDamping; } p{ in_damping };
		return invoke(mesh, fn, L"PrimitiveComponent", L"SetLinearDamping", p);
	}

	inline bool get_physics_linear_velocity(UObject* mesh, FName bone, FVector& out_vel)
	{
		static UFunction* fn = nullptr;
		struct { FName BoneName; FVector ReturnValue; } p{ bone, {} };
		if (!invoke(mesh, fn, L"PrimitiveComponent", L"GetPhysicsLinearVelocity", p))
			return false;
		out_vel = p.ReturnValue;
		return true;
	}

	inline bool set_physics_linear_velocity(UObject* mesh, FVector new_vel,
		bool add_to_current, FName bone)
	{
		static UFunction* fn = nullptr;
		struct
		{
			FVector NewVel;          // 0x00
			bool    bAddToCurrent;   // 0x18
			uint8_t Pad_19[0x3];
			FName   BoneName;        // 0x1C
			uint8_t Pad_24[0x4];
		} p{ new_vel, add_to_current, {}, bone, {} };
		static_assert(sizeof(p) == 0x28, "SetPhysicsLinearVelocity parms");
		return invoke(mesh, fn, L"PrimitiveComponent", L"SetPhysicsLinearVelocity", p);
	}

	inline bool get_physics_angular_velocity_in_radians(UObject* mesh, FName bone, FVector& out_ang_vel)
	{
		static UFunction* fn = nullptr;
		struct { FName BoneName; FVector ReturnValue; } p{ bone, {} };
		static_assert(sizeof(p) == 0x20, "GetPhysicsAngularVelocityInRadians parms");
		if (!invoke(mesh, fn, L"PrimitiveComponent", L"GetPhysicsAngularVelocityInRadians", p))
			return false;
		out_ang_vel = p.ReturnValue;
		return true;
	}

	inline bool set_physics_angular_velocity_in_radians(UObject* mesh, FVector new_ang_vel,
		bool add_to_current, FName bone)
	{
		static UFunction* fn = nullptr;
		struct
		{
			FVector NewAngVel;       // 0x00
			bool    bAddToCurrent;   // 0x18
			uint8_t Pad_19[0x3];
			FName   BoneName;        // 0x1C
			uint8_t Pad_24[0x4];
		} p{ new_ang_vel, add_to_current, {}, bone, {} };
		static_assert(sizeof(p) == 0x28, "SetPhysicsAngularVelocityInRadians parms");
		return invoke(mesh, fn, L"PrimitiveComponent", L"SetPhysicsAngularVelocityInRadians", p);
	}

	inline bool add_torque_in_radians(UObject* mesh, FVector torque, FName bone,
		bool accel_change)
	{
		static UFunction* fn = nullptr;
		struct
		{
			FVector Torque;          // 0x00
			FName   BoneName;        // 0x18
			bool    bAccelChange;    // 0x20
			uint8_t Pad_21[0x7];
		} p{ torque, bone, accel_change, {} };
		static_assert(sizeof(p) == 0x28, "AddTorqueInRadians parms");
		return invoke(mesh, fn, L"PrimitiveComponent", L"AddTorqueInRadians", p);
	}

	inline bool wake_rigid_body(UObject* mesh, FName bone)
	{
		static UFunction* fn = nullptr;
		struct { FName BoneName; } p{ bone };
		static_assert(sizeof(p) == 0x8, "WakeRigidBody parms");
		return invoke(mesh, fn, L"PrimitiveComponent", L"WakeRigidBody", p);
	}

	inline bool k2_get_actor_rotation(UObject* actor, FRotator& out_rotation)
	{
		static UFunction* fn = nullptr;
		struct { FRotator ReturnValue; } p{};
		static_assert(sizeof(p) == 0x18, "K2_GetActorRotation parms");
		if (!invoke(actor, fn, L"Actor", L"K2_GetActorRotation", p))
			return false;
		out_rotation = p.ReturnValue;
		return true;
	}

	inline bool get_control_rotation(UObject* controller, FRotator& out_rotation)
	{
		static UFunction* fn = nullptr;
		struct { FRotator ReturnValue; } p{};
		static_assert(sizeof(p) == 0x18, "GetControlRotation parms");
		if (!invoke(controller, fn, L"Controller", L"GetControlRotation", p))
			return false;
		out_rotation = p.ReturnValue;
		return true;
	}
}

#if defined(__clang__) || defined(__GNUC__)
	#pragma GCC diagnostic pop
#endif
