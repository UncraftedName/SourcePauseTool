/*
* Purpose: private shared stuff between the mesh builder and renderer. Helps keep the public header(s) relatively
* clean and can be used by any other internal rendering files. You should never have to include this file as a
* user unless you want to do something quirky like adding custom verts/indices.
* 
* Here's a basic overview of the system:
* 
* We want a system that allows the user to create a mesh made of lines, faces, and different colors. At the end of
* the day we need to call CMatRenderContextPtr->GetDynamicMesh() & CMatRenderContextPtr->CraeteStaticMesh(). The
* latter can be used (almost) as-is, but the former needs to be filled every single time it's rendered.
* 
*  ┌────────────────────┐                                       ┌───────────────────────┐
*  │CreateCollideFeature│                                       │       MeshData        │
*  └────────┬───────────┘                                       │┌─────────────────────┐│
*           │                                                   ││MeshComponentData(2x)││
*           │can be used for                                    ││    ┌──────────┐     ││
*           ▼                                                   ││    │vector of │     ││
*  ┌───────────────────┐ edits state of ┌───────────────────┐   ││    │VertexData│     ││
*  │MeshBuilderDelegate├───────────────►│MeshBuilderInternal├──►││    └──────────┘     ││
*  └───────────────────┘                └───────────────────┘   └┴┬────────────────────┴┘
*   (stateless) ▲                                                 │
*               │gives user a delegate                            │converted to a MeshUnit
*               │                                                 │- immediately for statics
*    ┌──────────┴───┐                                             │- during rendering for dynamics
*    │MeshBuilderPro│                                             ▼
*    └──────────────┘                                        ┌──────────────────┐
*     (stateless)                                            │     MeshUnit     │
*                                                            │┌────────────────┐│
*  ┌───────────────────┐          returned to user           ││IMeshWrapper(2x)││
*  │Static/Dynamic Mesh│◄────────────────────────────────────┤│    ┌──────┐    ││
*  └────────┬──────────┘                                     ││    │IMesh*│    ││
*           │                                                ││    └──────┘    ││
*           │user gives to delegate                          └┴────────────────┴┘
*           ▼
* ┌────────────────────┐        ┌────────────────────────────┐    ┌───────────────────────┐
* │MeshRendererDelegate├───────►│std::vector<MeshUnitWrapper>├───►│DrawOpaqueRenderables()│
* └────────────────────┘ queues └───────────────────────────┬┘    └───────────────────────┘
*  (stateless) ▲                                            │ rendered
*              │gives user a delegate                       │   ┌────────────────────────────┐
*              │                                            └──►│DrawTranslucentRenderables()│
*  ┌───────────┴───────┐ signals pre-frame ┌───────────┐        └────────────────────────────┘
*  │MeshRendererFeature│◄──────────────────┤spt_overlay│
*  └───────────────────┘                   └───────────┘
* 
* A brief description of some of the main classes:
* 
* MeshBuilderInternal - holds temporary MeshData buffers for static/dynamic meshes
* MeshComponentData - holds the verts & indices needed for mesh creation
* IMeshWrapper - a wrapper of the game's mesh representation, used to store or make IMesh* objects
* MeshUnit - a fully cooked and seasoned mesh ready for rendering (or has enough data to create one)
* MeshRendererFeature - handles loading/unloading of the whole system, rendering, and render callbacks
* MeshUnitWrapper - wraps static/dynamic meshes, also handles rendering and render callbacks
* 
* The purpose of having stateless classes is to keep public headers clean (state is managed by internal classes).
* The purpose of using std::function for mesh building/rendering is to enforce proper initialization and cleanup.
* The purpose of using delegates is for shorter variable names.
*/

#pragma once

#include "mesh_defs_public.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "mathlib\vector.h"
#include "materialsystem\imaterial.h"

#pragma warning(push)
#pragma warning(disable : 5054)
#include "materialsystem\imesh.h"
#pragma warning(pop)

#define VPROF_LEVEL 1
#ifndef SSDK2007
#define RAD_TELEMETRY_DISABLED
#endif
#include "vprof.h"

#define VPROF_BUDGETGROUP_MESHBUILDER _T("Mesh_Builder")
#define VPROF_BUDGETGROUP_MESH_RENDERER _T("Mesh_Renderer")

#include <stack>

#include "spt\utils\interfaces.hpp"
#include "source files\vector_slice.hpp"

#define MAX_MESH_VERTS 32767
#define MAX_MESH_INDICES 32767

// TODO COMMENTS
inline int g_meshRenderFrameNum = 0;
inline bool g_inMeshRenderSignal = false;

using VertIndex = unsigned short;
using DynamicMeshToken = DynamicMesh;

template<class T>
using VectorStack = std::stack<T, std::vector<T>>;

// a single vertex - may have position, color, uv coordinates, normal, etc
struct VertexData
{
	Vector pos;
	color32 col;

	VertexData(){};
	VertexData(const Vector& pos, color32 color) : pos(pos), col(color) {}
};

enum class MeshPrimitiveType
{
	Lines,
	Triangles,
	Count
};

struct MeshVertData
{
	VectorSlice<VertexData> verts;
	VectorSlice<VertIndex> indices;
	MeshPrimitiveType type;
	IMaterial* material;

	MeshVertData() = default;

	MeshVertData(MeshVertData&& other);

	MeshVertData(std::vector<VertexData>& vertDataVec,
	             std::vector<VertIndex>& vertIndexVec,
	             MeshPrimitiveType type,
	             IMaterial* material);

	bool Empty() const;

	~MeshVertData();
};

struct IMeshWrapper
{
	IMesh* iMesh;
	IMaterial* material;
};

struct DynamicMeshUnit
{
	VectorSlice<MeshVertData> vDataSlice;
	const MeshPositionInfo posInfo;

	DynamicMeshUnit(VectorSlice<MeshVertData>& vDataSlice, const MeshPositionInfo& posInfo);
	DynamicMeshUnit(const DynamicMeshUnit&) = delete;
	DynamicMeshUnit(DynamicMeshUnit&& other);
};

struct StaticMeshUnit
{
	IMeshWrapper* meshesArr;
	const size_t nMeshes;
	const MeshPositionInfo posInfo;

	StaticMeshUnit(size_t nMeshes, const MeshPositionInfo& posInfo);
	~StaticMeshUnit();
};

struct MeshComponent
{
	struct MeshUnitWrapper* unitWrapper;
	MeshVertData* vertData; // null for statics
	IMeshWrapper iMeshWrapper;

	std::weak_ordering operator<=>(const MeshComponent& rhs) const;
};

// the ranges are: [first, second)
using MeshComponentContainer = std::vector<MeshComponent>;
using ComponentRange = std::pair<MeshComponentContainer::iterator, MeshComponentContainer::iterator>;
using ConstComponentRange = std::pair<MeshComponentContainer::const_iterator, MeshComponentContainer::const_iterator>;

// TODO add functionality to convert to collides????? :eyes:
struct MeshBuilderInternal
{
	// you can't swap the vectors cuz they're used in slices
	std::list<std::vector<VertexData>> sharedVertArrays;
	std::list<std::vector<VertIndex>> sharedIndexArrays;
	std::vector<MeshVertData> sharedVertDataArrays;

	VectorSlice<MeshVertData> curMeshVertData;

	VectorStack<DynamicMeshUnit> dynamicMeshUnits;

	MeshVertData& FindOrAddVData(MeshPrimitiveType type, IMaterial* material);
	MeshPositionInfo _CalcPosInfoForCurrentMesh();

	struct
	{
		ConstComponentRange curRange;
		ConstComponentRange batchRange; // for dynamic meshes
		bool dynamic;
	} creationStatus;

	void BeginIMeshCreation(ConstComponentRange range, bool dynamic);
	IMeshWrapper _CreateIMeshFromRange(ConstComponentRange range,
	                                   size_t totalVerts,
	                                   size_t totalIndices,
	                                   bool dynamic);
	IMeshWrapper GetNextIMeshWrapper();

	void FrameCleanup();
	const DynamicMeshUnit& GetDynamicMeshFromToken(DynamicMeshToken token) const;
};

inline MeshBuilderInternal g_meshBuilderInternal;

struct MeshBuilderMatMgr
{
	IMaterial *matOpaque = nullptr, *matAlpha = nullptr, *matAlphaNoZ = nullptr;
	bool materialsInitialized = false;

	void Load();
	void Unload();
	IMaterial* GetMaterial(bool opaque, bool zTest, color32 colorMod);
};

inline MeshBuilderMatMgr g_meshMaterialMgr;

#endif
