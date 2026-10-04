#pragma once

#include <Windows.h>
#include <cstdint>
#include <cwchar>
#include <string>

/*
 * Minimal hand-maintained UE layer - the Dumper-7 dump is no longer a compile
 * dependency; only the constants below were harvested from it once.
 *
 * What this file provides:
 *   - core object layouts (UObject/UField/UStruct/UFunction) verified against
 *     live memory (InternalIndex/ClassName checks passed)
 *   - the GObjects chunked-array walk and the GWorld global
 *   - ProcessEvent through vtable slot kProcessEventIdx
 *   - FName -> string via the game's own AppendString (RVA)
 *   - FindFunction by class/function name (walks SuperStruct + Children)
 *   - typed flat structs for the handful of fields the DLL reads or writes
 *   - one ProcessEvent wrapper per function the DLL calls, each caching its
 *     UFunction* in a function-local static after the first lookup
 *
 * All offsets correspond to Ride-Win64-Shipping.exe 5.6.0-20702 rel-1.3;
 * the signature table in Signatures.hpp reports when that build changes.
 */

namespace UE
{
    // ---- global RVAs ----------------------------------------------------
    inline constexpr uintptr_t kGObjectsRva     = 0x0A50E640;
    inline constexpr uintptr_t kGWorldRva       = 0x0A272A50;
    inline constexpr uintptr_t kAppendStringRva = 0x01256FE0;
    inline constexpr int       kProcessEventIdx = 0x4C;

    inline void* ImageBase()
    {
        return GetModuleHandleW(nullptr);
    }

    // ---- math types -----------------------------------------------------
    struct FVector
    {
        double X = 0.0, Y = 0.0, Z = 0.0;
    };

    struct FRotator
    {
        double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
    };

    // ComparisonIndex 0 / Number 0 == NAME_None when value-initialised.
    struct FName
    {
        int32_t  Index  = 0;
        uint32_t Number = 0;
    };

    // Game-side FString the AppendString callee writes into.
    struct FString
    {
        wchar_t* Data;
        int32_t  Num;
        int32_t  Max;
    };

    // ---- core object layouts -------------------------------------------
    struct UFunction;

    struct UObject
    {
        void*    VTable;    // 0x00
        int32_t  Flags;     // 0x08 EObjectFlags
        int32_t  Index;     // 0x0C InternalIndex
        UObject* Class;     // 0x10
        FName    Name;      // 0x18
        UObject* Outer;     // 0x20
        // ends at 0x28

        void ProcessEvent(UFunction* Fn, void* Parms) const
        {
            using PE_t = void (*)(const UObject*, UFunction*, void*);
            void** VTable = *reinterpret_cast<void***>(const_cast<UObject*>(this));
            PE_t Pe = reinterpret_cast<PE_t>(VTable[kProcessEventIdx]);
            Pe(this, Fn, Parms);
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
        // ChildProperties 0x50, Size 0x58, ... - not needed
        // ends at 0x50 as far as this layer is concerned
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

    // ---- object array ---------------------------------------------------
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

    inline constexpr int32_t kElementsPerChunk = 0x10000;

    inline TUObjectArray* GObjects()
    {
        return reinterpret_cast<TUObjectArray*>(
            static_cast<uint8_t*>(ImageBase()) + kGObjectsRva);
    }

    inline UObject* GetByIndex(int32_t Index)
    {
        TUObjectArray* Arr = GObjects();
        if (Index < 0 || Index >= Arr->NumElements || !Arr->Objects)
            return nullptr;

        const int32_t ChunkIdx = Index / kElementsPerChunk;
        const int32_t InChunk  = Index % kElementsPerChunk;
        if (ChunkIdx >= Arr->NumChunks)
            return nullptr;

        FUObjectItem* Chunk = Arr->Objects[ChunkIdx];
        if (!Chunk)
            return nullptr;

        return Chunk[InChunk].Object;
    }

    // ---- globals --------------------------------------------------------
    // (GetWorld lives below, after the UWorld definition.)

    // ---- names ----------------------------------------------------------
    // Delegates to the game's FName::AppendString; the destination buffer is
    // large enough that the callee never reallocates it (same idiom the
    // Dumper-7 SDK itself uses).
    inline std::wstring NameToString(const FName& Name)
    {
        using Append_t = void (*)(const FName*, FString&);
        static const Append_t Append = reinterpret_cast<Append_t>(
            static_cast<uint8_t*>(ImageBase()) + kAppendStringRva);

        wchar_t Buffer[1024] = {};
        FString Str{ Buffer, 0, 1024 };
        Append(&Name, Str);

        // Do NOT trust Str.Num for the character count: the engine's
        // AppendString observedly includes the null terminator in it (a
        // 25-char name reported Num=26), which would embed a L'\0' into the
        // returned wstring and make every literal comparison fail. The buffer
        // is zero-initialized and UE keeps FString zero-terminated, so wcslen
        // yields the true length either way.
        const wchar_t* Data = Str.Data ? Str.Data : Buffer;
        return std::wstring(Data, Data + wcslen(Data));
    }

    inline bool NameEquals(const FName& Name, const wchar_t* Literal)
    {
        return NameToString(Name) == Literal;
    }

    // ---- class / function lookups ---------------------------------------
    inline bool IsA(const UObject* Obj, const wchar_t* ClassName)
    {
        if (!Obj || !Obj->Class)
            return false;

        for (UStruct* Clss = reinterpret_cast<UStruct*>(Obj->Class);
             Clss; Clss = Clss->SuperStruct)
        {
            if (NameEquals(Clss->Name, ClassName))
                return true;
        }
        return false;
    }

    // Mirrors the Dumper-7 GetFunction(ClassName, FuncName): walk the
    // SuperStruct chain for the declaring class, then its Children list for
    // the field of the requested name. Result is cached by the callers.
    inline UFunction* FindFunction(UObject* Context, const wchar_t* ClassName,
                                   const wchar_t* FuncName)
    {
        if (!Context || !Context->Class)
            return nullptr;

        for (UStruct* Clss = reinterpret_cast<UStruct*>(Context->Class);
             Clss; Clss = Clss->SuperStruct)
        {
            if (!NameEquals(Clss->Name, ClassName))
                continue;

            for (UField* Field = Clss->Children; Field; Field = Field->Next)
            {
                if (NameEquals(Field->Name, FuncName))
                    return reinterpret_cast<UFunction*>(Field);
            }
        }
        return nullptr;
    }

    // ---- typed structs (flat, padded to the dump's absolute offsets) ----
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

        void* At(int32_t I) const
        {
            return static_cast<void**>(Data)[I];
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

    // All interaction goes through ProcessEvent wrappers below; no fields read.
    struct UPrimitiveComponent : UObject
    {
    };

    // The Blueprint vehicle the flight code drives: fields by absolute offset.
    // Inherits APawn the way the real class hierarchy does (Pawn -> ... -> BP vehicle).
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

    inline UWorld* GetWorld()
    {
        return *reinterpret_cast<UWorld**>(
            static_cast<uint8_t*>(ImageBase()) + kGWorldRva);
    }

    // ---- ProcessEvent wrappers ------------------------------------------
    // Parms layouts follow the dump's parameter structs 1:1 (sizes asserted).
    // Each UFunction* is resolved once and cached in a function-local static;
    // a null cache (lookup failed) degrades to the zero value.

    inline bool IsGravityEnabled(UObject* Mesh)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Mesh, L"PrimitiveComponent", L"IsGravityEnabled");
        struct { bool ReturnValue; } P{};
        if (Fn) Mesh->ProcessEvent(Fn, &P);
        return P.ReturnValue;
    }

    inline void SetEnableGravity(UObject* Mesh, bool bEnable)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Mesh, L"PrimitiveComponent", L"SetEnableGravity");
        struct { bool bGravityEnabled; } P{ bEnable };
        if (Fn) Mesh->ProcessEvent(Fn, &P);
    }

    inline float GetLinearDamping(UObject* Mesh)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Mesh, L"PrimitiveComponent", L"GetLinearDamping");
        struct { float ReturnValue; } P{};
        if (Fn) Mesh->ProcessEvent(Fn, &P);
        return P.ReturnValue;
    }

    inline void SetLinearDamping(UObject* Mesh, float InDamping)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Mesh, L"PrimitiveComponent", L"SetLinearDamping");
        struct { float InDamping; } P{ InDamping };
        if (Fn) Mesh->ProcessEvent(Fn, &P);
    }

    inline FVector GetPhysicsLinearVelocity(UObject* Mesh, FName Bone)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Mesh, L"PrimitiveComponent", L"GetPhysicsLinearVelocity");
        struct { FName BoneName; FVector ReturnValue; } P{ Bone, {} };
        if (Fn) Mesh->ProcessEvent(Fn, &P);
        return P.ReturnValue;
    }

    inline void SetPhysicsLinearVelocity(UObject* Mesh, FVector NewVel,
                                         bool bAddToCurrent, FName Bone)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Mesh, L"PrimitiveComponent", L"SetPhysicsLinearVelocity");
        struct
        {
            FVector NewVel;          // 0x00
            bool    bAddToCurrent;   // 0x18
            uint8_t Pad_19[0x3];
            FName   BoneName;        // 0x1C
            uint8_t Pad_24[0x4];
        } P{ NewVel, bAddToCurrent, {}, Bone, {} };
        static_assert(sizeof(P) == 0x28, "SetPhysicsLinearVelocity parms");
        if (Fn) Mesh->ProcessEvent(Fn, &P);
    }

    inline FVector GetPhysicsAngularVelocityInRadians(UObject* Mesh, FName Bone)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Mesh, L"PrimitiveComponent", L"GetPhysicsAngularVelocityInRadians");
        struct { FName BoneName; FVector ReturnValue; } P{ Bone, {} };
        static_assert(sizeof(P) == 0x20, "GetPhysicsAngularVelocityInRadians parms");
        if (Fn) Mesh->ProcessEvent(Fn, &P);
        return P.ReturnValue;
    }

    inline void SetPhysicsAngularVelocityInRadians(UObject* Mesh, FVector NewAngVel,
                                                   bool bAddToCurrent, FName Bone)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Mesh, L"PrimitiveComponent", L"SetPhysicsAngularVelocityInRadians");
        struct
        {
            FVector NewAngVel;       // 0x00
            bool    bAddToCurrent;   // 0x18
            uint8_t Pad_19[0x3];
            FName   BoneName;        // 0x1C
            uint8_t Pad_24[0x4];
        } P{ NewAngVel, bAddToCurrent, {}, Bone, {} };
        static_assert(sizeof(P) == 0x28, "SetPhysicsAngularVelocityInRadians parms");
        if (Fn) Mesh->ProcessEvent(Fn, &P);
    }

    inline void AddTorqueInRadians(UObject* Mesh, FVector Torque, FName Bone,
                                   bool bAccelChange)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Mesh, L"PrimitiveComponent", L"AddTorqueInRadians");
        struct
        {
            FVector Torque;          // 0x00
            FName   BoneName;        // 0x18
            bool    bAccelChange;    // 0x20
            uint8_t Pad_21[0x7];
        } P{ Torque, Bone, bAccelChange, {} };
        static_assert(sizeof(P) == 0x28, "AddTorqueInRadians parms");
        if (Fn) Mesh->ProcessEvent(Fn, &P);
    }

    inline void WakeRigidBody(UObject* Mesh, FName Bone)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Mesh, L"PrimitiveComponent", L"WakeRigidBody");
        struct { FName BoneName; } P{ Bone };
        static_assert(sizeof(P) == 0x8, "WakeRigidBody parms");
        if (Fn) Mesh->ProcessEvent(Fn, &P);
    }

    inline FRotator K2_GetActorRotation(UObject* Actor)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Actor, L"Actor", L"K2_GetActorRotation");
        struct { FRotator ReturnValue; } P{};
        static_assert(sizeof(P) == 0x18, "K2_GetActorRotation parms");
        if (Fn) Actor->ProcessEvent(Fn, &P);
        return P.ReturnValue;
    }

    inline FRotator GetControlRotation(UObject* Controller)
    {
        static UFunction* Fn = nullptr;
        if (!Fn)
            Fn = FindFunction(Controller, L"Controller", L"GetControlRotation");
        struct { FRotator ReturnValue; } P{};
        static_assert(sizeof(P) == 0x18, "GetControlRotation parms");
        if (Fn) Controller->ProcessEvent(Fn, &P);
        return P.ReturnValue;
    }
}
