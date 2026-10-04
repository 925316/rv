#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "SDK/Engine_classes.hpp"
#include "SDK/AVS_Vehicle_classes.hpp"
#include "SDK/BP_VehicleBase_classes.hpp"
#include "Signatures.hpp"

// user32 is not in cl.exe's default libs.
#pragma comment(lib, "user32.lib")

/*
 * SHIFT           kinematic flight: own velocity, overwrites body velocity per step
 * W               forward accel while held (no target speed; tap = micro-adjust)
 * A / D           sideways, same model, combines with W
 * S               brake to a clean stop (wins over everything)
 * release SHIFT   gravity/damping restored; momentum carries into the landing
 * SPACE           game handbrake, never read
 *
 * UObject access on the game thread only; body-level writes, never the transform.
 */

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // cm/s, cm/s^2.
    constexpr double kMaxSpeed        = 10000.0;  // runaway clamp (100 m/s)
    constexpr double kRiseSpeed       = 150.0;    // climb speed held by the lift ramp
    constexpr double kLiftAccel       = 1200.0;   // ramp rate up to kRiseSpeed
    constexpr double kForwardAccel    = 1500.0;   // W, per second while held
    constexpr double kBrakeAccel      = 2500.0;   // S
    constexpr double kStrafeAccel     = 1200.0;   // A/D, per second

    // rad/s^2 (bAccelChange = true, so inertia does not enter).
    constexpr double kMaxAccel        = 20.0;     // total angular accel clamp
    constexpr double kLevelGain       = 6.0;      // per rad of pitch / roll error
    constexpr double kYawGain         = 4.0;      // per rad of yaw error
    constexpr double kDamp            = 2.5;      // per rad/s of angular velocity

    bool g_HasConsole = false;

    char g_LastLine[600] = "(no output yet)";   // last debug line, survives for the crash filter
    char g_CrashLogPath[MAX_PATH] = "RideFlight.crash.log";

    void DebugLine(const char* Fmt, ...)
    {
        char Buf[512];
        va_list Args;
        va_start(Args, Fmt);
        const int Written = vsnprintf(Buf, sizeof(Buf) - 2, Fmt, Args);
        va_end(Args);

        if (Written <= 0)
            return;

        strcat_s(Buf, "\n");

        strncpy_s(g_LastLine, Buf, _TRUNCATE);
        for (int I = (int)strlen(g_LastLine) - 1; I >= 0 && (g_LastLine[I] == '\n' || g_LastLine[I] == '\r'); --I)
            g_LastLine[I] = '\0';

        if (g_HasConsole)
        {
            fputs(Buf, stdout);
            fflush(stdout);
        }
        OutputDebugStringA(Buf);
    }

    LONG WINAPI CrashFilter(EXCEPTION_POINTERS* Ep)
    {
        const EXCEPTION_RECORD* Er = Ep->ExceptionRecord;

        char ModulePath[MAX_PATH] = "unknown-module";
        unsigned long long ModBase = 0;
        HMODULE Mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)Er->ExceptionAddress, &Mod) && Mod)
        {
            GetModuleFileNameA(Mod, ModulePath, MAX_PATH);
            ModBase = (unsigned long long)(uintptr_t)Mod;
        }

        char Line[1200];
        snprintf(Line, sizeof(Line),
                 "[crash] code=0x%08lX addr=%p module=%s+0x%llX | last=%s\r\n",
                 Er->ExceptionCode, Er->ExceptionAddress, ModulePath,
                 (unsigned long long)((unsigned long long)(uintptr_t)Er->ExceptionAddress - ModBase),
                 g_LastLine);

        FILE* F = nullptr;
        if (fopen_s(&F, g_CrashLogPath, "a") == 0 && F)
        {
            fputs(Line, F);
            fclose(F);
        }
        OutputDebugStringA(Line);

        return EXCEPTION_EXECUTE_HANDLER;
    }

    void InitCrashLog(HMODULE Self)
    {
        char Path[MAX_PATH] = {};
        if (Self && GetModuleFileNameA(Self, Path, MAX_PATH))
            snprintf(g_CrashLogPath, sizeof(g_CrashLogPath), "%s.crash.log", Path);

        SetUnhandledExceptionFilter(CrashFilter);
    }

    // Scan the EXE for every native function the DLL anchors on, once at
    // startup. A not-found means the game updated and the table wants a
    // refresh; a moved RVA with the pattern still matching means code slid
    // but the signature holds - both cases are reported instead of failing
    // silently later.
    void VerifySignatures()
    {
        const uint8_t* ImageBase = (const uint8_t*)GetModuleHandleW(nullptr);
        int Found = 0;
        constexpr int Total = (int)(sizeof(Signatures::kTable) / sizeof(Signatures::kTable[0]));

        for (const Signatures::KnownSignature& Sig : Signatures::kTable)
        {
            Signatures::Pattern P;
            if (!Signatures::Parse(Sig.Text, P))
            {
                DebugLine("[sig] %-25s parse error", Sig.Name);
                continue;
            }

            const uint8_t* Hit = Signatures::FindInImage(P);
            if (!Hit)
            {
                DebugLine("[sig] %-25s NOT FOUND - game update?", Sig.Name);
                continue;
            }

            ++Found;
            const uint32_t Rva = (uint32_t)(Hit - ImageBase);
            if (Rva == Sig.Rva)
                DebugLine("[sig] %-25s ok rva 0x%X", Sig.Name, Rva);
            else
                DebugLine("[sig] %-25s moved rva 0x%X (table 0x%X)", Sig.Name, Rva, Sig.Rva);
        }

        DebugLine("[sig] %d/%d signatures resolved", Found, Total);
    }

    bool KeyDown(int VirtualKey)
    {
        return (GetAsyncKeyState(VirtualKey) & 0x8000) != 0;
    }

    double Axis(int PositiveKey, int NegativeKey)
    {
        return (KeyDown(PositiveKey) ? 1.0 : 0.0) - (KeyDown(NegativeKey) ? 1.0 : 0.0);
    }

    double WrapAxis(double Angle)
    {
        // Same result as FRotator::NormalizeAxis, kept local so no extra function body is linked.
        Angle = std::fmod(Angle, 360.0);
        if (Angle < 0.0)
            Angle += 360.0;
        return Angle > 180.0 ? Angle - 360.0 : Angle;
    }

    // UE rotation matrix columns, written out to avoid linking GetActorForwardVector etc.
    void BasisFromRotator(const SDK::FRotator& Rotation, SDK::FVector& Forward, SDK::FVector& Right, SDK::FVector& Up)
    {
        const double Pitch = Rotation.Pitch * kPi / 180.0;
        const double Yaw   = Rotation.Yaw   * kPi / 180.0;

        const double CP = std::cos(Pitch), SP = std::sin(Pitch);
        const double CY = std::cos(Yaw),   SY = std::sin(Yaw);

        Forward = { CP * CY, CP * SY, SP };
        Right   = { -SY,     CY,      0.0 };
        Up      = { -SP * CY, -SP * SY, CP };
    }

    struct FlightState
    {
        SDK::ABP_VehicleBase_C*  Vehicle = nullptr;
        SDK::UPrimitiveComponent* Mesh   = nullptr;
        bool  bEngaged              = false;
        bool  bSavedGravity         = true;
        bool  bSavedBlockDownForce  = false;
        SDK::FRotator TakeoffAttitude{};   // latched at Engage, held all flight
        SDK::FVector KinVel{};             // our integrated velocity
        double SavedSpringDownforce   = 0.0; // AVS state saved at Engage,
        double SavedBaseLinearDrag    = 0.0; // restored on Disengage
        double SavedDefaultLinearDrag = 0.0;
        double SavedLinearDamping     = 0.0;
        bool   SavedDynamicAirDrag    = false;
        ULONGLONG LastStepMs          = 0;   // integrator dt
    };

    FlightState g_Flight;

    // g_World: level-transition detector, not a cache; the live world is re-fetched every tick.
    SDK::UWorld* g_World       = nullptr;
    HWND         g_GameWnd     = nullptr;
    WNDPROC      g_OrigWndProc = nullptr;

    constexpr UINT_PTR kTickTimerId  = 0x52494445; // arbitrary, non-zero
    constexpr UINT     kTickPeriodMs = 8;          // 8 ms cadence

    SDK::APlayerController* LocalController(SDK::UWorld* World)
    {
        SDK::UGameInstance* GameInstance = World->OwningGameInstance;
        if (!GameInstance || GameInstance->LocalPlayers.Num() == 0)
            return nullptr;

        return GameInstance->LocalPlayers[0]->PlayerController;
    }

    SDK::ABP_VehicleBase_C* LocalVehicle(SDK::APlayerController* Controller)
    {
        SDK::APawn* Pawn = Controller ? Controller->Pawn : nullptr;

        // Blueprint classes are matched with StaticName(), not StaticClass().
        if (!Pawn || !Pawn->IsA(SDK::ABP_VehicleBase_C::StaticName()))
            return nullptr;

        return static_cast<SDK::ABP_VehicleBase_C*>(Pawn);
    }

    void Engage(SDK::ABP_VehicleBase_C* Vehicle, SDK::UPrimitiveComponent* Mesh)
    {
        g_Flight.bSavedGravity        = Mesh->IsGravityEnabled();
        g_Flight.bSavedBlockDownForce = Vehicle->BlockDownwardForceInAir;

        // Park drag/damping at zero: they would ripple the step between overwrites.
        g_Flight.SavedSpringDownforce    = Vehicle->SpringDownforce;
        g_Flight.SavedBaseLinearDrag     = Vehicle->BaseLinearDrag;
        g_Flight.SavedDefaultLinearDrag  = Vehicle->DefaultLinearDrag;
        g_Flight.SavedDynamicAirDrag     = Vehicle->DynamicAirDrag;
        g_Flight.SavedLinearDamping      = Mesh->GetLinearDamping();
        Vehicle->SpringDownforce   = 0.0;
        Vehicle->BaseLinearDrag    = 0.0;
        Vehicle->DefaultLinearDrag = 0.0;
        Vehicle->DynamicAirDrag    = false;
        Mesh->SetLinearDamping(0.0f);

        // Seed integrator with body velocity; latch the pose held for the whole flight.
        const SDK::FVector Velocity0 = Mesh->GetPhysicsLinearVelocity(SDK::FName());
        g_Flight.KinVel = Velocity0;
        g_Flight.TakeoffAttitude = Vehicle->K2_GetActorRotation();
        g_Flight.LastStepMs = GetTickCount64();

        Mesh->SetEnableGravity(false);
        Vehicle->BlockDownwardForceInAir = true;

        DebugLine("[RideFlight] engage (sim stays on, gravity was %d, blockDownForce was %d, pose=%.1f/%.1f/%.1f, vel=(%.0f %.0f %.0f))",
            g_Flight.bSavedGravity ? 1 : 0, g_Flight.bSavedBlockDownForce ? 1 : 0,
            g_Flight.TakeoffAttitude.Pitch, g_Flight.TakeoffAttitude.Yaw, g_Flight.TakeoffAttitude.Roll,
            g_Flight.KinVel.X, g_Flight.KinVel.Y, g_Flight.KinVel.Z);

        g_Flight.Vehicle  = Vehicle;
        g_Flight.Mesh     = Mesh;
        g_Flight.bEngaged = true;
    }

    void Disengage(SDK::ABP_VehicleBase_C* CurrentVehicle, SDK::UPrimitiveComponent* CurrentMesh)
    {
        if (!g_Flight.bEngaged)
            return;

        // Pointers may be stale after respawn/swap; restoring into freed objects crashes.
        const bool bStale = (g_Flight.Vehicle != CurrentVehicle) || (g_Flight.Mesh != CurrentMesh);
        if (bStale)
        {
            DebugLine("[RideFlight] disengage: saved pointers stale (vehicle %p -> %p), skipping restore",
                (void*)g_Flight.Vehicle, (void*)CurrentVehicle);
            g_Flight = FlightState{};
            return;
        }

        if (g_Flight.Mesh)
        {
            // No velocity restore: body already carries our last KinVel.
            g_Flight.Mesh->SetEnableGravity(g_Flight.bSavedGravity);
            g_Flight.Mesh->SetLinearDamping(static_cast<float>(g_Flight.SavedLinearDamping));
            g_Flight.Mesh->SetPhysicsAngularVelocityInRadians({ 0.0, 0.0, 0.0 }, false, SDK::FName());
            g_Flight.Mesh->WakeRigidBody(SDK::FName());
        }

        if (g_Flight.Vehicle)
        {
            g_Flight.Vehicle->BlockDownwardForceInAir = g_Flight.bSavedBlockDownForce;
            g_Flight.Vehicle->SpringDownforce   = g_Flight.SavedSpringDownforce;
            g_Flight.Vehicle->BaseLinearDrag    = g_Flight.SavedBaseLinearDrag;
            g_Flight.Vehicle->DefaultLinearDrag = g_Flight.SavedDefaultLinearDrag;
            g_Flight.Vehicle->DynamicAirDrag    = g_Flight.SavedDynamicAirDrag;
        }

        DebugLine("[RideFlight] disengage (body already at vel=(%.0f %.0f %.0f), restored gravity=%d, blockDownForce=%d)",
            g_Flight.KinVel.X, g_Flight.KinVel.Y, g_Flight.KinVel.Z,
            g_Flight.bSavedGravity ? 1 : 0, g_Flight.bSavedBlockDownForce ? 1 : 0);

        g_Flight = FlightState{};
    }

    void Step(SDK::ABP_VehicleBase_C* Vehicle, SDK::UPrimitiveComponent* Mesh, SDK::APlayerController* Controller)
    {
        const SDK::FName Bone;                       // NAME_None, value-initialised by FName()
        const SDK::FRotator Control = Controller->GetControlRotation();
        const SDK::FRotator Actor   = Vehicle->K2_GetActorRotation();

        // Clamp dt: debugger pauses must not integrate a huge jump.
        const ULONGLONG NowMs = GetTickCount64();
        double Dt = (NowMs - g_Flight.LastStepMs) * 0.001;
        g_Flight.LastStepMs = NowMs;
        if (Dt > 0.1)
            Dt = 0.1;
        if (Dt <= 0.0)
            Dt = 0.008;

        // Re-zero every tick: the Blueprint re-asserts gravity, down-force and drag.
        Mesh->SetEnableGravity(false);
        Vehicle->BlockDownwardForceInAir = true;
        Vehicle->SpringDownforce   = 0.0;
        Vehicle->BaseLinearDrag    = 0.0;
        Vehicle->DefaultLinearDrag = 0.0;
        Vehicle->DynamicAirDrag    = false;
        Mesh->SetLinearDamping(0.0f);

        // Forward from camera yaw only; pitch ignored so looking down does not dive.
        const double CamYaw = Control.Yaw * kPi / 180.0;
        const double Fx = std::cos(CamYaw), Fy = std::sin(CamYaw);

        double VelX = g_Flight.KinVel.X;
        double VelY = g_Flight.KinVel.Y;
        double VelZ = g_Flight.KinVel.Z;

        if (KeyDown('S'))
        {
            const double HorizSpeed = std::sqrt(VelX * VelX + VelY * VelY);
            if (HorizSpeed <= 20.0)
            {
                VelX = 0.0;
                VelY = 0.0;
            }
            else
            {
                double Step = kBrakeAccel * Dt;
                if (Step > HorizSpeed - 20.0)
                    Step = HorizSpeed - 20.0;
                const double Scale = (HorizSpeed - Step) / HorizSpeed;
                VelX *= Scale;
                VelY *= Scale;
            }
        }
        else
        {
            if (KeyDown('W'))
            {
                VelX += Fx * kForwardAccel * Dt;
                VelY += Fy * kForwardAccel * Dt;
            }

            const double Strafe = Axis('D', 'A');
            if (Strafe != 0.0)
            {
                VelX += -Fy * Strafe * kStrafeAccel * Dt;
                VelY +=  Fx * Strafe * kStrafeAccel * Dt;
            }

            const double NewSpeed = std::sqrt(VelX * VelX + VelY * VelY);
            if (NewSpeed > kMaxSpeed)
            {
                const double Scale = kMaxSpeed / NewSpeed;
                VelX *= Scale;
                VelY *= Scale;
            }
        }

        if (VelZ < kRiseSpeed)
        {
            VelZ += kLiftAccel * Dt;
            if (VelZ > kRiseSpeed)
                VelZ = kRiseSpeed;
        }

        g_Flight.KinVel.X = VelX;
        g_Flight.KinVel.Y = VelY;
        g_Flight.KinVel.Z = VelZ;

        // Absolute overwrite of the body velocity; never the transform - that races
        // the game thread.
        Mesh->WakeRigidBody(Bone);
        Mesh->SetPhysicsLinearVelocity({ VelX, VelY, VelZ }, false, Bone);

        // Error against the latched takeoff pose, not level or camera yaw.
        // UE positive pitch is a NEGATIVE right-hand rotation about Right (roll
        // mirrors it), so pitch/roll torque opposes the error sign. Yaw about +Z is as-is.
        const double PitchErr = (g_Flight.TakeoffAttitude.Pitch - Actor.Pitch) * kPi / 180.0;
        const double RollErr  = (g_Flight.TakeoffAttitude.Roll  - Actor.Roll)  * kPi / 180.0;
        const double YawErr   =  WrapAxis(g_Flight.TakeoffAttitude.Yaw - Actor.Yaw) * kPi / 180.0;

        SDK::FVector Fwd, Right, Up;
        BasisFromRotator(Actor, Fwd, Right, Up);

        const SDK::FVector AngVel = Mesh->GetPhysicsAngularVelocityInRadians(Bone);

        SDK::FVector Torque;
        Torque.X = -Right.X * (PitchErr * kLevelGain) - Fwd.X * (RollErr * kLevelGain) - AngVel.X * kDamp;
        Torque.Y = -Right.Y * (PitchErr * kLevelGain) - Fwd.Y * (RollErr * kLevelGain) - AngVel.Y * kDamp;
        Torque.Z = -Right.Z * (PitchErr * kLevelGain) - Fwd.Z * (RollErr * kLevelGain) - AngVel.Z * kDamp
                 + YawErr * kYawGain;

        const double Magnitude = std::sqrt(Torque.X * Torque.X + Torque.Y * Torque.Y + Torque.Z * Torque.Z);
        if (Magnitude > kMaxAccel)
        {
            const double Scale = kMaxAccel / Magnitude;
            Torque.X *= Scale;
            Torque.Y *= Scale;
            Torque.Z *= Scale;
        }

        Mesh->AddTorqueInRadians(Torque, Bone, true);
    }

    // Game-thread pump at >= 8 ms spacing; all UObject access stays in this path.
    void FlightTick()
    {
        static ULONGLONG LastTick = 0;
        const ULONGLONG Now = GetTickCount64();
        if (Now - LastTick < kTickPeriodMs)
            return;
        LastTick = Now;

        // Re-fetch every tick; the level swap can leave it null or half-initialized.
        SDK::UWorld* World = SDK::UWorld::GetWorld();
        if (!World || !World->PersistentLevel || !World->OwningGameInstance)
            return;

        if (World != g_World)
        {
            Disengage(nullptr, nullptr);   // old-world pointers, stale now
            g_World = World;
            DebugLine("[RideFlight] world changed -> %p, re-acquired", (void*)World);
        }

        SDK::APlayerController*   Controller = LocalController(World);
        SDK::ABP_VehicleBase_C*   Vehicle    = LocalVehicle(Controller);
        SDK::UPrimitiveComponent* Mesh       = Vehicle ? Vehicle->VehicleMesh : nullptr;

        if (!KeyDown(VK_SHIFT) || !Vehicle || !Mesh || !Controller)
        {
            Disengage(Vehicle, Mesh);
            return;
        }

        if (!g_Flight.bEngaged || g_Flight.Vehicle != Vehicle)
        {
            Disengage(Vehicle, Mesh);
            Engage(Vehicle, Mesh);
        }

        Step(Vehicle, Mesh, Controller);
    }

    LRESULT CALLBACK HookWndProc(HWND Hwnd, UINT Msg, WPARAM Wp, LPARAM Lp)
    {
        if (Msg == WM_TIMER && Wp == kTickTimerId)
        {
            FlightTick();
            return 0;
        }

        FlightTick();   // backup pump in case the timer message is coalesced away

        // g_OrigWndProc can be null for messages dispatched before the assignment below.
        if (g_OrigWndProc)
            return CallWindowProcW(g_OrigWndProc, Hwnd, Msg, Wp, Lp);
        return DefWindowProcW(Hwnd, Msg, Wp, Lp);
    }

    struct WindowSearch
    {
        DWORD Pid;
        HWND  Found;
        bool  bGotUnrealClass;
    };

    BOOL CALLBACK FindGameWindowProc(HWND Hwnd, LPARAM Lparam)
    {
        WindowSearch* S = reinterpret_cast<WindowSearch*>(Lparam);

        DWORD Pid = 0;
        GetWindowThreadProcessId(Hwnd, &Pid);
        if (Pid != S->Pid || !IsWindowVisible(Hwnd))
            return TRUE;

        char Class[64] = {};
        GetClassNameA(Hwnd, Class, sizeof(Class) - 1);

        // Prefer UE's window class; any visible window of this process as fallback
        // (the debug console lives in conhost, excluded by the PID filter).
        if (strcmp(Class, "UnrealWindow") == 0)
        {
            S->Found = Hwnd;
            S->bGotUnrealClass = true;
            return FALSE;
        }
        if (!S->bGotUnrealClass && S->Found == nullptr)
            S->Found = Hwnd;
        return TRUE;
    }

    HWND FindGameWindow()
    {
        WindowSearch S = { GetCurrentProcessId(), nullptr, false };
        EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&S));
        return S.Found;
    }

    // Subclass the game window; false if no window yet, caller retries.
    bool InstallHook()
    {
        if (g_GameWnd && IsWindow(g_GameWnd))
            return true;

        HWND Hwnd = FindGameWindow();
        if (!Hwnd)
            return false;

        WNDPROC Prev = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(Hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
        if (!Prev)
        {
            DebugLine("[RideFlight] subclass of %p failed (%lu)", (void*)Hwnd, GetLastError());
            return false;
        }

        // g_OrigWndProc before SetTimer so the first tick already passes messages on.
        g_OrigWndProc = Prev;
        g_GameWnd = Hwnd;
        SetTimer(Hwnd, kTickTimerId, kTickPeriodMs, nullptr);

        DebugLine("[RideFlight] game-thread tick installed (hwnd=%p, timer=%u ms)", (void*)Hwnd, kTickPeriodMs);
        return true;
    }
}

DWORD MainThread(HMODULE Module)
{
    InitCrashLog(Module);   // crash filter first, before any console exists

    FILE* ConsoleOut = nullptr;
    if (AllocConsole())
        freopen_s(&ConsoleOut, "CONOUT$", "w", stdout);
    SetConsoleTitleA("RideFlight debug");
    g_HasConsole = (ConsoleOut != nullptr);

    DebugLine("[RideFlight] waiting for game window ... (build " __DATE__ " " __TIME__ ")");

    VerifySignatures();

    bool bInstalled = false;
    bool bRetriedLogged = false;
    while (true)
    {
        if (!bInstalled)
        {
            bInstalled = InstallHook();
            if (bInstalled)
                continue;

            if (!bRetriedLogged)
            {
                bRetriedLogged = true;
                DebugLine("[RideFlight] no game window yet, retrying quietly ...");
            }
        }
        else if (!g_GameWnd || !IsWindow(g_GameWnd))
        {
            DebugLine("[RideFlight] game window lost, re-installing hook ...");
            g_GameWnd = nullptr;
            g_OrigWndProc = nullptr;
            bInstalled = false;
        }

        Sleep(250);
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        CreateThread(0, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, 0);
        break;
    }

    return TRUE;
}
