#pragma once

#include "..\mesh_defs.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "mathlib\vector.h"
#include "materialsystem\imaterial.h"

#pragma warning(push)
#pragma warning(disable : 5054)
#include "materialsystem\imesh.h"
#pragma warning(pop)

#include "spt\utils\mesh_utils.hpp"

#include <stack>
#include <tracy\Tracy.hpp>
#include <tracy\TracyC.h>

inline const char* SPT_TRACY_MESH_THREAD_GROUP_NAME = "mesh thread";
#define SPT_TRACY_MESH_THREAD_GROUP_ID MAKEID('M', 'E', 'S', 'H')
// #define SPT_TRACY_MESH_SET_THREAD_NAME() tracy::SetThreadNameWithHint(SPT_TRACY_MESH_THREAD_GROUP_NAME, SPT_TRACY_MESH_THREAD_GROUP_ID)
#define SPT_TRACY_MESH_SET_THREAD_NAME() (void)0;

using VertIndex = utils::MbCompactMesh::idx_type;
using DynamicMeshToken = DynamicMesh;

template<class T>
using VectorStack = std::stack<T, std::vector<T>>;

#endif
