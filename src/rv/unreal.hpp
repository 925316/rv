#pragma once

#include <Windows.h>
#include <cstdint>
#include <cwchar>
#include <string>

/*
 * Hand-maintained UE layer; the Dumper-7 dump is no longer a compile
 * dependency (only its constants were harvested once). Layouts below were
 * verified against live memory.
 * Offsets: target game build 5.6.0-20702 rel-1.3; signatures.hpp
 * reports when the build changes.
 */

namespace unreal
{
	// Global RVAs (5.6.0-20702 rel-1.3): compile-time fallbacks. At startup
	// resolve.hpp's rv::resolve_globals() overwrites the mutable g_* counterparts;
	// all accessors below read the g_* copies so a game update no longer means
	// a rebuild.
	inline constexpr uintptr_t G_OBJECTS_RVA     = 0x0A50E640;
	inline constexpr uintptr_t G_WORLD_RVA       = 0x0A272A50;
	inline constexpr uintptr_t APPEND_STRING_RVA = 0x01256FE0;
	inline constexpr int       PROCESS_EVENT_IDX = 0x4C;

	// Runtime-resolved counterparts of the constants above. Written exactly
	// once by rv::resolve_globals() on the loader thread BEFORE the
	// game-thread hook is installed, then read-only from the hook. Never
	// mutate them once hooks are live - that would race the game thread.
	inline uintptr_t g_objects_rva       = G_OBJECTS_RVA;
	inline uintptr_t g_world_rva         = G_WORLD_RVA;
	inline uintptr_t g_append_string_rva = APPEND_STRING_RVA;

	inline void* image_base()
	{
		return GetModuleHandleW(nullptr);
	}

	// Math types
	struct FVector
	{
		double X = 0.0, Y = 0.0, Z = 0.0;
	};

	struct FRotator
	{
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
	};

	// 0/0 value-initialised == NAME_None.
	struct FName
	{
		int32_t  Index  = 0;
		uint32_t Number = 0;
	};

	// Written into by the game's AppendString callee.
	struct FString
	{
		wchar_t* Data;
		int32_t  Num;
		int32_t  Max;
	};

	// Core object layouts
	struct UFunction;

	struct UObject
	{
		void*    VTable;    // 0x00
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
		UField*  Children;          // 0x48
		// ChildProperties 0x50, Size 0x58 - not needed
	};
	static_assert(offsetof(UStruct, SuperStruct) == 0x40, "UStruct layout");
	static_assert(offsetof(UStruct, Children) == 0x48, "UStruct layout");

	struct UFunction : UStruct
	{
		uint8_t  Pad_50[0x60];       // 0x50 .. 0xB0
		uint32_t FunctionFlags;      // 0xB0 EFunctionFlags
		uint8_t  Pad_B4[0x24];       // 0xB4 .. 0xD8
		void*    ExecFunction;       // 0xD8 native thunk / VM entry
	};
	static_assert(sizeof(UFunction) == 0xE0, "UFunction layout");

	// Object array
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

	inline UObject* get_by_index(int32_t index)
	{
		TUObjectArray* arr = g_objects();
		if (index < 0 || index >= arr->NumElements || !arr->Objects)
			return nullptr;

		const int32_t chunk_idx = index / ELEMENTS_PER_CHUNK;
		const int32_t in_chunk  = index % ELEMENTS_PER_CHUNK;
		if (chunk_idx >= arr->NumChunks)
			return nullptr;

		FUObjectItem* chunk = arr->Objects[chunk_idx];
		if (!chunk)
			return nullptr;

		return chunk[in_chunk].Object;
	}

	// Names
	// Delegates to the game's FName::AppendString (buffer is never reallocated).
	inline std::wstring name_to_string(const FName& name)
	{
		using Append_t = void (*)(const FName*, FString&);
		const Append_t append = reinterpret_cast<Append_t>(
			static_cast<uint8_t*>(image_base()) + g_append_string_rva);

		wchar_t buffer[1024] = {};
		FString str{ buffer, 0, 1024 };
		append(&name, str);

		// str.Num is NOT the character count: AppendString includes the null
		// terminator in it (25-char name reports Num=26). Use wcslen instead.
		const wchar_t* data = str.Data ? str.Data : buffer;
		return std::wstring(data, data + wcslen(data));
	}

	inline bool name_equals(const FName& name, const wchar_t* literal)
	{
		return name_to_string(name) == literal;
	}

	// Class / function lookups
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
	// Result is cached by the callers.
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

	// Typed structs (flat, padded to absolute offsets)
	struct UWorld : UObject
	{
		uint8_t  Pad_28[0x30 - 0x28];
		UObject* PersistentLevel;        // 0x30
		uint8_t  Pad_38[0x228 - 0x38];
		UObject* OwningGameInstance;     // 0x228
	};
	static_assert(offsetof(UWorld, PersistentLevel) == 0x30, "UWorld layout");
	static_assert(offsetof(UWorld, OwningGameInstance) == 0x228, "UWorld layout");

	struct TArray
	{
		void*   Data;
		int32_t Num;
		int32_t Max;

		void* at(int32_t i) const
		{
			return static_cast<void**>(Data)[i];
		}
	};

	struct UGameInstance : UObject
	{
		uint8_t Pad_28[0x38 - 0x28];
		TArray  LocalPlayers;             // 0x38
	};
	static_assert(offsetof(UGameInstance, LocalPlayers) == 0x38, "UGameInstance layout");

	struct ULocalPlayer : UObject
	{
		uint8_t  Pad_28[0x30 - 0x28];
		UObject* PlayerController;        // 0x30
	};
	static_assert(offsetof(ULocalPlayer, PlayerController) == 0x30, "ULocalPlayer layout");

	struct APlayerController : UObject
	{
		uint8_t  Pad_28[0x2E8 - 0x28];
		UObject* Pawn;                    // 0x2E8
	};
	static_assert(offsetof(APlayerController, Pawn) == 0x2E8, "AController layout");

	struct APawn : UObject
	{
	};

	// All interaction via ProcessEvent wrappers below; no fields read.
	struct UPrimitiveComponent : UObject
	{
	};

	// Blueprint vehicle: fields by absolute offset, inherits APawn like the
	// real hierarchy (Pawn -> ... -> BP vehicle).
	struct AVehicleBase : APawn
	{
		uint8_t  Pad_28[0x4C0 - 0x28];
		UObject* VehicleMesh;                 // 0x04C0 (UStaticMeshComponent*)
		uint8_t  Pad_4C8[0x708 - 0x4C8];
		bool     DynamicAirDrag;              // 0x0708
		uint8_t  Pad_709[0x7];
		double   BaseLinearDrag;              // 0x0710
		double   DefaultLinearDrag;           // 0x0718
		uint8_t  Pad_720[0xB38 - 0x720];
		double   SpringDownforce;             // 0x0B38
		uint8_t  Pad_B40[0xE28 - 0xB40];
		bool     BlockDownwardForceInAir;     // 0x0E28
	};
	static_assert(offsetof(AVehicleBase, VehicleMesh) == 0x4C0, "VehicleMesh layout");
	static_assert(offsetof(AVehicleBase, DynamicAirDrag) == 0x708, "DynamicAirDrag layout");
	static_assert(offsetof(AVehicleBase, BaseLinearDrag) == 0x710, "BaseLinearDrag layout");
	static_assert(offsetof(AVehicleBase, DefaultLinearDrag) == 0x718, "DefaultLinearDrag layout");
	static_assert(offsetof(AVehicleBase, SpringDownforce) == 0xB38, "SpringDownforce layout");
	static_assert(offsetof(AVehicleBase, BlockDownwardForceInAir) == 0xE28, "BlockDownwardForceInAir layout");

	inline UWorld* get_world()
	{
		return *reinterpret_cast<UWorld**>(
			static_cast<uint8_t*>(image_base()) + g_world_rva);
	}

	// ProcessEvent wrappers: par layouts match the dump's structs (sizes
	// asserted); each UFunction* is cached in a function-local static, and a
	// failed lookup degrades to the zero value.
	inline bool is_gravity_enabled(UObject* mesh)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(mesh, L"PrimitiveComponent", L"IsGravityEnabled");
		struct { bool ReturnValue; } p{};
		if (fn) mesh->process_event(fn, &p);
		return p.ReturnValue;
	}

	inline void set_enable_gravity(UObject* mesh, bool enable)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(mesh, L"PrimitiveComponent", L"SetEnableGravity");
		struct { bool bGravityEnabled; } p{ enable };
		if (fn) mesh->process_event(fn, &p);
	}

	inline float get_linear_damping(UObject* mesh)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(mesh, L"PrimitiveComponent", L"GetLinearDamping");
		struct { float ReturnValue; } p{};
		if (fn) mesh->process_event(fn, &p);
		return p.ReturnValue;
	}

	inline void set_linear_damping(UObject* mesh, float in_damping)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(mesh, L"PrimitiveComponent", L"SetLinearDamping");
		struct { float InDamping; } p{ in_damping };
		if (fn) mesh->process_event(fn, &p);
	}

	inline FVector get_physics_linear_velocity(UObject* mesh, FName bone)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(mesh, L"PrimitiveComponent", L"GetPhysicsLinearVelocity");
		struct { FName BoneName; FVector ReturnValue; } p{ bone, {} };
		if (fn) mesh->process_event(fn, &p);
		return p.ReturnValue;
	}

	inline void set_physics_linear_velocity(UObject* mesh, FVector new_vel,
	                                        bool add_to_current, FName bone)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(mesh, L"PrimitiveComponent", L"SetPhysicsLinearVelocity");
		struct
		{
			FVector NewVel;          // 0x00
			bool    bAddToCurrent;   // 0x18
			uint8_t Pad_19[0x3];
			FName   BoneName;        // 0x1C
			uint8_t Pad_24[0x4];
		} p{ new_vel, add_to_current, {}, bone, {} };
		static_assert(sizeof(p) == 0x28, "SetPhysicsLinearVelocity parms");
		if (fn) mesh->process_event(fn, &p);
	}

	inline FVector get_physics_angular_velocity_in_radians(UObject* mesh, FName bone)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(mesh, L"PrimitiveComponent", L"GetPhysicsAngularVelocityInRadians");
		struct { FName BoneName; FVector ReturnValue; } p{ bone, {} };
		static_assert(sizeof(p) == 0x20, "GetPhysicsAngularVelocityInRadians parms");
		if (fn) mesh->process_event(fn, &p);
		return p.ReturnValue;
	}

	inline void set_physics_angular_velocity_in_radians(UObject* mesh, FVector new_ang_vel,
	                                                    bool add_to_current, FName bone)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(mesh, L"PrimitiveComponent", L"SetPhysicsAngularVelocityInRadians");
		struct
		{
			FVector NewAngVel;       // 0x00
			bool    bAddToCurrent;   // 0x18
			uint8_t Pad_19[0x3];
			FName   BoneName;        // 0x1C
			uint8_t Pad_24[0x4];
		} p{ new_ang_vel, add_to_current, {}, bone, {} };
		static_assert(sizeof(p) == 0x28, "SetPhysicsAngularVelocityInRadians parms");
		if (fn) mesh->process_event(fn, &p);
	}

	inline void add_torque_in_radians(UObject* mesh, FVector torque, FName bone,
	                                  bool accel_change)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(mesh, L"PrimitiveComponent", L"AddTorqueInRadians");
		struct
		{
			FVector Torque;          // 0x00
			FName   BoneName;        // 0x18
			bool    bAccelChange;    // 0x20
			uint8_t Pad_21[0x7];
		} p{ torque, bone, accel_change, {} };
		static_assert(sizeof(p) == 0x28, "AddTorqueInRadians parms");
		if (fn) mesh->process_event(fn, &p);
	}

	inline void wake_rigid_body(UObject* mesh, FName bone)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(mesh, L"PrimitiveComponent", L"WakeRigidBody");
		struct { FName BoneName; } p{ bone };
		static_assert(sizeof(p) == 0x8, "WakeRigidBody parms");
		if (fn) mesh->process_event(fn, &p);
	}

	inline FRotator k2_get_actor_rotation(UObject* actor)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(actor, L"Actor", L"K2_GetActorRotation");
		struct { FRotator ReturnValue; } p{};
		static_assert(sizeof(p) == 0x18, "K2_GetActorRotation parms");
		if (fn) actor->process_event(fn, &p);
		return p.ReturnValue;
	}

	inline FRotator get_control_rotation(UObject* controller)
	{
		static UFunction* fn = nullptr;
		if (!fn)
			fn = find_function(controller, L"Controller", L"GetControlRotation");
		struct { FRotator ReturnValue; } p{};
		static_assert(sizeof(p) == 0x18, "GetControlRotation parms");
		if (fn) controller->process_event(fn, &p);
		return p.ReturnValue;
	}
}
