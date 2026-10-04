#pragma once

#include <Windows.h>

#include "input.hpp"
#include "log.hpp"
#include "math.hpp"
#include "state.hpp"
#include "unreal.hpp"

/*
 * Kinematic flight model: engage latches the AVS snapshot and the body
 * velocity, step() overwrites the body velocity and adds attitude torque, and
 * disengage puts the AVS back the way it was found.
 *
 * UObject access happens on the game thread only, and only through this path.
 * Body-level writes, never the transform - writing the transform races the
 * game thread and visibly snaps the vehicle.
 */

namespace rv
{
	namespace helper
	{
		inline unreal::APlayerController* get_controller(unreal::UWorld* world)
		{
			unreal::UGameInstance* game_instance = static_cast<unreal::UGameInstance*>(world->OwningGameInstance);
			if (!game_instance)
				return nullptr;

			// Num > 0 with Data == null shows up across a level transition, and
			// at() is a raw index into Data: dereferencing it ends the process.
			const unreal::TArray& players = game_instance->LocalPlayers;
			if (!players.Data || players.Num <= 0)
				return nullptr;

			unreal::ULocalPlayer* local = static_cast<unreal::ULocalPlayer*>(players.at(0));
			if (!local)
				return nullptr;

			return static_cast<unreal::APlayerController*>(local->PlayerController);
		}

		inline unreal::AVehicleBase* get_vehicle(unreal::APlayerController* controller)
		{
			unreal::APawn* pawn = controller ? static_cast<unreal::APawn*>(controller->Pawn) : nullptr;

			// Matched by name through the class chain.
			if (!pawn || !unreal::is_a(pawn, L"BP_VehicleBase_C"))
				return nullptr;

			return static_cast<unreal::AVehicleBase*>(pawn);
		}

		inline void engage(unreal::AVehicleBase* vehicle, unreal::UPrimitiveComponent* mesh)
		{
			// A failed UFunction lookup returns zeroed parms, and that zero used to be
			// saved as "gravity was off" / "damping was 0" and written straight
			// back on disengage. Refuse to engage rather than restore a value we
			// never actually read.
			bool  saved_gravity = false;
			float saved_damping = 0.0f;
			if (!unreal::is_gravity_enabled(mesh, saved_gravity) ||
				!unreal::get_linear_damping(mesh, saved_damping))
			{
				static bool reported = false;
				log::debug_once(reported, "engage aborted: UFunction lookup failed on mesh %p", (void*)mesh);
				return;
			}

			// Same argument for the two reads the flight is seeded from: a
			// zeroed parm latches an upright attitude and a stationary velocity,
			// which launches the vehicle from rest and drops the nose.
			const unreal::FName bone;
			unreal::FVector    velocity_0{};
			unreal::FRotator   attitude{};
			if (!unreal::get_physics_linear_velocity(mesh, bone, velocity_0) ||
				!unreal::k2_get_actor_rotation(vehicle, attitude))
			{
				static bool reported = false;
				log::debug_once(reported, "engage aborted: seed read failed on mesh %p / vehicle %p",
					(void*)mesh, (void*)vehicle);
				return;
			}

			g_state.m_saved_gravity = saved_gravity;
			g_state.m_saved_linear_damping = saved_damping;
			g_state.m_saved_block_down_force = vehicle->BlockDownwardForceInAir;

			// Zero drag/damping: they ripple between overwrites.
			g_state.m_saved_spring_downforce = vehicle->SpringDownforce;
			g_state.m_saved_base_linear_drag = vehicle->BaseLinearDrag;
			g_state.m_saved_default_linear_drag = vehicle->DefaultLinearDrag;
			g_state.m_saved_dynamic_air_drag = vehicle->DynamicAirDrag;
			vehicle->SpringDownforce = 0.0;
			vehicle->BaseLinearDrag = 0.0;
			vehicle->DefaultLinearDrag = 0.0;
			vehicle->DynamicAirDrag = false;
			unreal::set_linear_damping(mesh, 0.0f);

			// Seed vel from body; latch current pose for the flight.
			g_state.m_kin_vel = velocity_0;
			g_state.m_takeoff_attitude = attitude;
			g_state.m_last_step_ms = GetTickCount64();

			unreal::set_enable_gravity(mesh, false);
			vehicle->BlockDownwardForceInAir = true;

			log::debug("engage (sim stays on, gravity was %d, blockDownForce was %d, pose=%.1f/%.1f/%.1f, vel=(%.0f %.0f %.0f))",
				g_state.m_saved_gravity ? 1 : 0, g_state.m_saved_block_down_force ? 1 : 0,
				g_state.m_takeoff_attitude.Pitch, g_state.m_takeoff_attitude.Yaw, g_state.m_takeoff_attitude.Roll,
				g_state.m_kin_vel.X, g_state.m_kin_vel.Y, g_state.m_kin_vel.Z);

			g_state.m_vehicle = vehicle;
			g_state.m_mesh = mesh;
			g_state.m_engaged = true;
		}

		inline void disengage()
		{
			if (!g_state.m_engaged)
				return;

			// The restore writes into the SAVED pointers, and the gate for that
			// is "is this still a live object", not "does it equal the current
			// vehicle/mesh". Comparing against the current ones skipped the
			// restore for vehicles that were alive and well - a mesh swap or a
			// world change left them with gravity off and drag zeroed for the
			// rest of the session, and the next engage then saved the polluted
			// values as if they were the originals.
			const bool mesh_live = unreal::is_valid(g_state.m_mesh);
			const bool vehicle_live = unreal::is_valid(g_state.m_vehicle);
			if ((g_state.m_mesh && !mesh_live) || (g_state.m_vehicle && !vehicle_live))
				log::debug("disengage: vehicle %p / mesh %p not live, restoring only the live ones",
					(void*)g_state.m_vehicle, (void*)g_state.m_mesh);

			bool restore_ok = true;
			if (g_state.m_mesh && mesh_live)
			{
				// No vel restore: body already runs our last m_kin_vel.
				restore_ok = unreal::set_enable_gravity(g_state.m_mesh, g_state.m_saved_gravity) && restore_ok;
				restore_ok = unreal::set_linear_damping(g_state.m_mesh, static_cast<float>(g_state.m_saved_linear_damping)) && restore_ok;
				restore_ok = unreal::set_physics_angular_velocity_in_radians(g_state.m_mesh, { 0.0, 0.0, 0.0 }, false, {}) && restore_ok;
				restore_ok = unreal::wake_rigid_body(g_state.m_mesh, {}) && restore_ok;
			}

			if (g_state.m_vehicle && vehicle_live)
			{
				g_state.m_vehicle->BlockDownwardForceInAir = g_state.m_saved_block_down_force;
				g_state.m_vehicle->SpringDownforce = g_state.m_saved_spring_downforce;
				g_state.m_vehicle->BaseLinearDrag = g_state.m_saved_base_linear_drag;
				g_state.m_vehicle->DefaultLinearDrag = g_state.m_saved_default_linear_drag;
				g_state.m_vehicle->DynamicAirDrag = g_state.m_saved_dynamic_air_drag;
			}

			if (!restore_ok)
			{
				static bool reported = false;
				log::debug_once(reported, "disengage: a restore write failed - the vehicle may keep gravity off");
			}

			log::debug("disengage (body already at vel=(%.0f %.0f %.0f), saved gravity=%d blockDownForce=%d, restored mesh=%d vehicle=%d)",
				g_state.m_kin_vel.X, g_state.m_kin_vel.Y, g_state.m_kin_vel.Z,
				g_state.m_saved_gravity ? 1 : 0, g_state.m_saved_block_down_force ? 1 : 0,
				(mesh_live && g_state.m_mesh) ? 1 : 0, (vehicle_live && g_state.m_vehicle) ? 1 : 0);

			g_state = state{};
		}

		inline void step(unreal::AVehicleBase* vehicle, unreal::UPrimitiveComponent* mesh, unreal::APlayerController* controller)
		{
			const unreal::FName bone;                       // NAME_None

			// Everything the control law needs is read BEFORE the first write.
			// A zeroed parm here is not a harmless default: a phantom upright
			// attitude with no damping term makes the vehicle tumble, and doing
			// it after the gravity/drag re-zero would leave it in a half-modified
			// state. Skipping the tick leaves the vehicle flying untouched.
			unreal::FRotator control{};
			unreal::FRotator actor{};
			if (!unreal::get_control_rotation(controller, control) ||
				!unreal::k2_get_actor_rotation(vehicle, actor))
			{
				static bool reported = false;
				log::debug_once(reported, "step skipped: rotation read failed");
				return;
			}

			unreal::FVector ang_vel{};
			if (!unreal::get_physics_angular_velocity_in_radians(mesh, bone, ang_vel))
			{
				static bool reported = false;
				log::debug_once(reported, "step skipped: angular velocity read failed");
				return;
			}

			// Clamp dt: debugger pauses must not integrate a huge jump.
			const ULONGLONG now_ms = GetTickCount64();
			double dt = (now_ms - g_state.m_last_step_ms) * 0.001;
			g_state.m_last_step_ms = now_ms;
			if (dt > 0.1)
				dt = 0.1;
			if (dt <= 0.0)
				dt = 0.008;

			// BP re-asserts gravity/downforce/drag each tick - re-zero first.
			bool writes_ok = unreal::set_enable_gravity(mesh, false);
			vehicle->BlockDownwardForceInAir = true;
			vehicle->SpringDownforce = 0.0;
			vehicle->BaseLinearDrag = 0.0;
			vehicle->DefaultLinearDrag = 0.0;
			vehicle->DynamicAirDrag = false;
			writes_ok = unreal::set_linear_damping(mesh, 0.0f) && writes_ok;

			// Forward from camera yaw only; pitch ignored so looking down does not dive.
			const double cam_yaw = control.Yaw * PI / 180.0;
			const double fx = std::cos(cam_yaw), fy = std::sin(cam_yaw);

			double vel_x = g_state.m_kin_vel.X;
			double vel_y = g_state.m_kin_vel.Y;
			double vel_z = g_state.m_kin_vel.Z;

			if (is_key_down('S'))
			{
				const double horiz_speed = std::sqrt(vel_x * vel_x + vel_y * vel_y);
				if (horiz_speed <= 20.0)
				{
					vel_x = 0.0;
					vel_y = 0.0;
				}
				else
				{
					double brake_step = BRAKE_ACCEL * dt;
					if (brake_step > horiz_speed - 20.0)
						brake_step = horiz_speed - 20.0;
					const double scale = (horiz_speed - brake_step) / horiz_speed;
					vel_x *= scale;
					vel_y *= scale;
				}
			}
			else
			{
				if (is_key_down('W'))
				{
					vel_x += fx * FORWARD_ACCEL * dt;
					vel_y += fy * FORWARD_ACCEL * dt;
				}

				const double strafe = get_axis('D', 'A');
				if (strafe != 0.0)
				{
					vel_x += -fy * strafe * STRAFE_ACCEL * dt;
					vel_y += fx * strafe * STRAFE_ACCEL * dt;
				}

				limit_length(vel_x, vel_y, MAX_SPEED);   // runaway clamp
			}

			vel_z += LIFT_ACCEL * dt;
			limit_length(vel_z, MAX_SPEED);

			g_state.m_kin_vel.X = vel_x;
			g_state.m_kin_vel.Y = vel_y;
			g_state.m_kin_vel.Z = vel_z;

			// Overwrite body velocity; never transform - that races the game thread.
			writes_ok = unreal::wake_rigid_body(mesh, bone) && writes_ok;
			writes_ok = unreal::set_physics_linear_velocity(mesh, { vel_x, vel_y, vel_z }, false, bone) && writes_ok;

			// Torque about the latched attitude, not the camera. UE +pitch =
			// negative rotation about Right and +roll mirrors it -> flip those;
			// yaw about +Z is as-is.
			const double pitch_err = (g_state.m_takeoff_attitude.Pitch - actor.Pitch) * PI / 180.0;
			const double roll_err = (g_state.m_takeoff_attitude.Roll - actor.Roll) * PI / 180.0;
			const double yaw_err = wrap_axis(g_state.m_takeoff_attitude.Yaw - actor.Yaw) * PI / 180.0;

			unreal::FVector fwd, right, up;
			get_basis(actor, fwd, right, up);

			unreal::FVector torque;
			torque.X = -right.X * (pitch_err * LEVEL_GAIN) - fwd.X * (roll_err * LEVEL_GAIN) - ang_vel.X * DAMP;
			torque.Y = -right.Y * (pitch_err * LEVEL_GAIN) - fwd.Y * (roll_err * LEVEL_GAIN) - ang_vel.Y * DAMP;
			torque.Z = -right.Z * (pitch_err * LEVEL_GAIN) - fwd.Z * (roll_err * LEVEL_GAIN) - ang_vel.Z * DAMP
				+ yaw_err * YAW_GAIN;

			limit_length(torque.X, torque.Y, torque.Z, MAX_ACCEL);

			writes_ok = unreal::add_torque_in_radians(mesh, torque, bone, true) && writes_ok;

			if (!writes_ok)
			{
				static bool reported = false;
				log::debug_once(reported, "step: a body write failed - flight is degraded this tick");
			}
		}
	}
}