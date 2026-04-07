#pragma once

// include this file before any monocle files to allow implicit conversions between monocle/SDK

#include "mathlib/vector.h"
#include "mathlib/mathlib.h"
#include "mathlib/vmatrix.h"
#include "mathlib/vplane.h"

#include <bit>

#define MON_VECTOR_CLASS_EXTRA \
	constexpr Vector(const ::Vector& v) : x(v.x), y(v.y), z(v.z) {} \
	operator const ::Vector&() const \
	{ \
		return reinterpret_cast<const ::Vector&>(*this); \
	}

#define MON_QANGLE_CLASS_EXTRA \
	constexpr QAngle(const ::QAngle& v) : x(v.x), y(v.y), z(v.z) {} \
	operator const ::QAngle&() const \
	{ \
		return reinterpret_cast<const ::QAngle&>(*this); \
	}

#define MON_MATRIX3X4_CLASS_EXTRA \
	constexpr matrix3x4_t(const ::matrix3x4_t& m) \
	{ \
		*this = std::bit_cast<matrix3x4_t>(m); \
	} \
	operator const ::matrix3x4_t&() const \
	{ \
		return reinterpret_cast<const ::matrix3x4_t&>(*this); \
	}

#define MON_VMATRIX_CLASS_EXTRA \
	constexpr VMatrix(const ::VMatrix& vm) \
	{ \
		for (int i = 0; i < 4; ++i) \
			for (int j = 0; j < 4; ++j) \
				m[i][j] = vm.m[i][j]; \
	} \
	operator const ::VMatrix&() const \
	{ \
		return reinterpret_cast<const ::VMatrix&>(*this); \
	}

#define MON_VPLANE_CLASS_EXTRA \
	constexpr VPlane(const ::VPlane& plane) : n(plane.m_Normal), d(plane.m_Dist) {} \
	operator const ::VPlane&() const \
	{ \
		return reinterpret_cast<const ::VPlane&>(*this); \
	}
