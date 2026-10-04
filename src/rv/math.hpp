#pragma once

#include <cmath>

#include "unreal.hpp"

/*
 * Pure flight math. No engine memory is touched here, so these stay
 * host-testable and are the first place to look when the handling feels
 * wrong. The basis is written out by hand to avoid linking engine getters.
 */

namespace rv
{
	namespace helper
	{
		constexpr double PI = 3.14159265358979323846;

		// Same result as FRotator::NormalizeAxis, kept local.
		inline double wrap_axis(double angle)
		{
			angle = std::fmod(angle, 360.0);
			if (angle < 0.0)
				angle += 360.0;
			return angle > 180.0 ? angle - 360.0 : angle;
		}

		inline void get_basis(const unreal::FRotator& rotation, unreal::FVector& forward, unreal::FVector& right, unreal::FVector& up)
		{
			const double pitch = rotation.Pitch * PI / 180.0;
			const double yaw = rotation.Yaw * PI / 180.0;

			const double cp = std::cos(pitch), sp = std::sin(pitch);
			const double cy = std::cos(yaw), sy = std::sin(yaw);

			forward = { cp * cy, cp * sy, sp };
			right = { -sy,     cy,      0.0 };
			up = { -sp * cy, -sp * sy, cp };
		}

		// Scale a single axis down to max_abs if it exceeds it. Returns true when
		// a clamp was applied.
	inline bool limit_length(double& v, double max_abs)
		{
			if (v > max_abs)
			{
				v = max_abs;
				return true;
			}
			if (v < -max_abs)
			{
				v = -max_abs;
				return true;
			}
			return false;
		}

		// Scale a 2D vector down to max_len if it exceeds it. Returns true when
		// a clamp was applied. Both the runaway clamp and the brake share this
		// instead of open-coding the sqrt/scale pair.
		inline bool limit_length(double& x, double& y, double max_len)
		{
			const double len = std::sqrt(x * x + y * y);
			if (len <= max_len || len <= 0.0)
				return false;

			const double scale = max_len / len;
			x *= scale;
			y *= scale;
			return true;
		}

		inline bool limit_length(double& x, double& y, double& z, double max_len)
		{
			const double len = std::sqrt(x * x + y * y + z * z);
			if (len <= max_len || len <= 0.0)
				return false;

			const double scale = max_len / len;
			x *= scale;
			y *= scale;
			z *= scale;
			return true;
		}
	}
}