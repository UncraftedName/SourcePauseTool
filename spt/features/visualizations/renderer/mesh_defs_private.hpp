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

// this allows arrays shared by all dynamic meshes; behaves kind of a stack - you can only edit the last slice
template<typename T>
struct VectorSlice
{
	std::vector<T>* vec;
	size_t off;
	size_t len;

	// TODO TRY SETTING TO DEFAULT
	VectorSlice() = default;

	VectorSlice(VectorSlice&) = delete;

	VectorSlice(VectorSlice&& other) : vec(other.vec), off(other.off), len(other.len)
	{
		other.vec = nullptr;
		other.off = other.len = 0;
	}

	VectorSlice(std::vector<T>& vec) : vec(&vec), off(vec.size()), len(0) {}

	/*void init(std::vector<T>& vec_)
	{
		vec = &vec_;
		off = vec_.size();
		len = 0;
	}*/

	VectorSlice<T>& operator=(VectorSlice<T>&& other)
	{
		if (&other == this)
			return *this;
		vec = other.vec;
		off = other.off;
		len = other.len;
		other.vec = nullptr;
		other.off = other.len = 0;
		return *this;
	};

	void pop()
	{
		if (vec)
		{
			Assert(off + len == vec->size());
			vec->resize(vec->size() - len);
			vec = nullptr;
			off = len = 0;
		}
	}

	inline std::vector<T>::iterator begin() const
	{
		return vec->begin() + off;
	}

	inline std::vector<T>::iterator end() const
	{
		return vec->begin() + off + len;
	}

	template<class _It>
	inline void add_range(_It first, _It last)
	{
		size_t dist = std::distance(first, last);
		reserve_extra(dist);
		vec->insert(vec->end(), first, last);
		len += dist;
	}

	inline void reserve_extra(size_t n)
	{
		if (vec->capacity() < off + len + n)
			vec->reserve(SmallestPowerOfTwoGreaterOrEqual(off + len + n));
	}

	inline void resize(size_t n)
	{
		vec->resize(off + n);
		len = n;
	}

	template<class _Pr>
	inline void erase_if(_Pr pred)
	{
		auto it = std::remove_if(begin(), end(), pred);
		auto removed = std::distance(it, end());
		vec->erase(it, end());
		len -= removed;
	}

	inline void push_back(const T& elem)
	{
		Assert(off + len == vec->size());
		vec->push_back(elem);
		len++;
	}

	template<class... Params>
	inline void emplace_back(Params&&... params)
	{
		Assert(off + len == vec->size());
		vec->emplace_back(std::forward<Params>(params)...);
		len++;
	}

	inline T& operator[](size_t i) const
	{
		return vec->at(off + i);
	}

	inline bool empty() const
	{
		return len == 0;
	}

	inline size_t size() const
	{
		return len;
	}

	~VectorSlice()
	{
		pop();
	}
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

	MeshVertData(MeshVertData&& other)
	    : verts(std::move(other.verts))
	    , indices(std::move(other.indices))
	    , type(other.type)
	    , material(other.material)
	{
		other.material = nullptr;
	}

	MeshVertData(std::vector<VertexData>& vertDataVec,
	             std::vector<VertIndex>& vertIndexVec,
	             MeshPrimitiveType type,
	             IMaterial* material)
	    : verts(vertDataVec), indices(vertIndexVec), type(type), material(material)
	{
		material->IncrementReferenceCount();
	}

	bool Empty() const
	{
		return verts.size() == 0 || indices.size() == 0;
	}

	~MeshVertData()
	{
		if (material)
			material->DecrementReferenceCount();
	}
};

struct DynamicMeshUnit
{
	VectorSlice<MeshVertData> vDataSlice;
	const MeshPositionInfo posInfo;

	DynamicMeshUnit(VectorSlice<MeshVertData>& vDataSlice, const MeshPositionInfo& posInfo)
	    : vDataSlice(std::move(vDataSlice)), posInfo(posInfo)
	{
	}

	DynamicMeshUnit(const DynamicMeshUnit&) = delete;
	DynamicMeshUnit(DynamicMeshUnit&& other) : vDataSlice(std::move(other.vDataSlice)), posInfo(other.posInfo) {}
};

struct IMeshWrapper
{
	IMesh* iMesh;
	IMaterial* material;
};

struct StaticMeshUnit
{
	IMeshWrapper* meshesArr;
	const size_t nMeshes;
	const MeshPositionInfo posInfo;

	~StaticMeshUnit()
	{
		if (meshesArr)
		{
			for (size_t i = 0; i < nMeshes; i++)
			{
				meshesArr[i].material->DecrementReferenceCount();
				CMatRenderContextPtr(interfaces::materialSystem)->DestroyStaticMesh(meshesArr[i].iMesh);
			}
			delete[] meshesArr;
		}
	}
};

struct MeshComponent
{
	struct MeshUnitWrapper* unitWrapper;
	MeshVertData* vertData; // null for statics
	IMeshWrapper iMeshWrapper;

	std::weak_ordering operator<=>(const MeshComponent& rhs) const;
};

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

	using MeshComponentIterator = const MeshVertData**;

	struct
	{
		MeshComponentIterator from, to;                     // [from,to)
		MeshComponentIterator lastBatchStart, lastBatchEnd; // for debug meshes
		bool dynamic;
	} creationStatus;

	IMeshWrapper CreateIMeshFromVertData(MeshComponentIterator from,
	                                     MeshComponentIterator to,
	                                     size_t totalVerts,
	                                     size_t totalIndices,
	                                     bool dynamic);
	IMeshWrapper CreateIMeshFromVertData(const MeshVertData& vData, bool dynamic);
	void BeginDynamicMeshCreation(MeshComponentIterator from, MeshComponentIterator to, bool dynamic);
	IMeshWrapper GetNextIMeshWrapper();

	void FrameCleanup();
	MeshPositionInfo _CalcPosInfoForCurrentMesh();
	const DynamicMeshUnit& GetDynamicMeshFromToken(DynamicMesh token) const;
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
