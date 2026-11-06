// Minimal quaternion / vector helpers (x, y, z, w; right-handed, -Z forward like LibOVR and OpenXR).
#pragma once

#include <array>
#include <cmath>

namespace od::ovr::math
{
using quat = std::array<float, 4>;
using vec3 = std::array<float, 3>;
using mat3 = std::array<std::array<float, 3>, 3>; // row-major

inline quat conjugate(const quat & q) { return {-q[0], -q[1], -q[2], q[3]}; }

inline quat mul(const quat & a, const quat & b)
{
	return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
	        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

inline vec3 add(const vec3 & a, const vec3 & b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline vec3 sub(const vec3 & a, const vec3 & b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline vec3 scale(const vec3 & a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }

inline vec3 rotate(const quat & q, const vec3 & v)
{
	quat p{v[0], v[1], v[2], 0};
	quat r = mul(mul(q, p), conjugate(q));
	return {r[0], r[1], r[2]};
}

inline mat3 to_mat3(const quat & q)
{
	auto [x, y, z, w] = q;
	return {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
	         {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
	         {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
}

inline quat normalize(const quat & q)
{
	float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	return n > 0 ? quat{q[0] / n, q[1] / n, q[2] / n, q[3] / n} : quat{0, 0, 0, 1};
}

// rotation by angular velocity w (rad/s) over dt seconds, applied in world space
inline quat integrate(const quat & q, const vec3 & w, float dt)
{
	float angle = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * dt;
	if (angle < 1e-6f)
		return q;
	float s = std::sin(angle / 2) / (angle / dt);
	quat d{w[0] * s, w[1] * s, w[2] * s, std::cos(angle / 2)};
	return normalize(mul(d, q));
}

inline quat yaw(float radians) { return {0, std::sin(radians / 2), 0, std::cos(radians / 2)}; }
} // namespace od::ovr::math
