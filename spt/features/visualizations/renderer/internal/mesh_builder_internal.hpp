#pragma once

#include "internal_defs.hpp"
#include "materials_manager.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include <forward_list>
#include <memory_resource>
#include <span>

/*
* For lack of a better place, the life cycle of meshes is described here. Grab a cookie and make some tea.
* 
* This file contains most of the wrappers I use of the game's IMesh* system, as well as the internal mesh builder.
* Like the mesh renderer, there are 3 mesh builders:
* - MeshBuilderPro (spt_meshBuilder)
* - MeshBuilderDelegate
* - MeshBuilderInternal (g_meshBuilderInternal)
* The MeshBuilderPro is stateless, and will give the user a stateless delegate when they want to create a mesh.
* The user then calls the various methods of the delegate which will edit the state of the internal builder.
* 
* At the end of the day, the mesh builder's purpose is to create IMesh* objects so that the renderer can call
* IMesh->Draw(). For static meshes, this is simple enough - the user can fill up the builder's buffers with data,
* and when they're done we can call IMatRenderContext::CreateStaticMesh(...). The IMesh* is then yeeted onto the
* heap and given to the renderer later. The life cycle for static meshes looks like this:
* 
* 1) The user calls MeshBuilderPro::CreateStaticMesh(...)
* 2) The MeshBuiderPro gives the user a MeshBuilderDelegate
* 3) The MeshBuilderDelegate edits the state of the MeshBuilderInternal, populating its current MeshVertData
* 4) The MeshBuilderPro asks the MeshBuilderInternal to create an IMesh* object from the MeshVertData
* 5) The MeshBuilderPro puts the IMesh* object into the heap (wrapped in a StaticMesh) and gives that to the user
* 6) The user can then destroy or give that StaticMesh to the MeshRendererFeature
* 
* Dynamic meshes are a little bit more complicated. Under the hood (in game code) there is only one dynamic mesh,
* and we simply edit its state. However, that means that only a single dynamic IMesh* object can exist at a time.
* This means that we must keep the instructions to build dynamic meshes for the entire frame, and recreate IMesh*
* objects just as they're about to be drawn. The life cycle for dynamic meshes looks like this:
* 
* 1) The user calls MeshBuilderPro::CreateDynamicMesh(...)
* 2) Same as above
* 3) Same as above
* 4) The MeshBuilderPro asks to the MeshBuilderInternal to save the current MeshVertData
* 5) The MeshBuilderPro gives the user a token for that data (only internal classes can use it)
* 6) The user can give that token to the renderer
* 7) The renderer asks the MeshBuilderInternal to create the IMesh* object when it's about to draw it (every time)
* 8) At the start of the next frame, the MeshBuilderInternal clears its MeshVertData arrays
* 
* As such, the user is never responsible for the destruction of dynamic meshes. There are a few optimizations,
* probably the most complicated of which being the fusing of dynamic meshes. When asking the MeshBuilderInternal
* for an IMesh*, the renderer provides not just 1 but a whole interval of meshes, then the builder will
* automatically fuse consecutive elements and iteratively construct a single IMesh* object at a time until it's
* done iterating over that interval.
*/

using VertexData = utils::MbColoredVert;

enum class MeshPrimitiveType : unsigned char
{
	Lines,
	Triangles,
	Count
};

// we will set aside buffers for the simple component types, meshes with other types will be allocated separately
constexpr size_t MAX_SIMPLE_COMPONENTS = ((size_t)MeshPrimitiveType::Count * (size_t)MeshMaterialSimple::Count);
#define SIMPLE_COMPONENT_INDEX(type, matType) ((size_t)(type) * (size_t)MeshMaterialSimple::Count + (size_t)(matType))

// does not handle deletion!
struct IMeshWrapper
{
	IMesh* iMesh;
	MaterialRef material;
	bool dynamic;

	~IMeshWrapper()
	{
		if (iMesh && !dynamic)
		{
			CMatRenderContextPtr context{interfaces::materialSystem};
			context->DestroyStaticMesh(iMesh);
		}
	}
};

class StaticMeshUnit
{
	IMeshWrapper* meshesArr;
	const size_t nMeshes;
	const MeshPositionInfo posInfo;

	StaticMeshUnit(size_t nMeshes, const MeshPositionInfo& posInfo)
	    : meshesArr(new IMeshWrapper[nMeshes]), nMeshes(nMeshes), posInfo(posInfo)
	{
	}

	~StaticMeshUnit()
	{
		for (size_t i = 0; i < nMeshes; i++)
			CMatRenderContextPtr(interfaces::materialSystem)->DestroyStaticMesh(meshesArr[i].iMesh);
		delete[] meshesArr;
	}
};

struct MbMeshBufs
{
	struct L1
	{
		std::pmr::vector<VertexData> verts{std::pmr::null_memory_resource()};
		std::pmr::vector<VertIndex> indices{std::pmr::null_memory_resource()};

		MeshPrimitiveType type;
		MaterialRef matRef;
	};

	struct TmpMesh
	{
		std::array<L1, MAX_SIMPLE_COMPONENTS> components;
		MeshPositionInfo posInfo;

		void Clear() {}
	} tmpMesh;

	using L2 = std::pmr::vector<L1>;
	using L3 = std::pmr::vector<L2>;

	L3 allBufs;

	std::reference_wrapper<std::pmr::memory_resource> pmr{*std::pmr::null_memory_resource()};

	void Init(std::pmr::memory_resource& newPmr)
	{
		L3 tmp(&newPmr);
		allBufs.swap(tmp);

		for (size_t i = 0; i < MAX_SIMPLE_COMPONENTS; i++)
		{
			L1 newElem{
			    .verts{&newPmr},
			    .indices{&newPmr},
			    .type = (MeshPrimitiveType)(i / (size_t)MeshMaterialSimple::Count),
			    .matRef = g_meshMaterialMgr.GetMaterial(
			        (MeshMaterialSimple)(i % (size_t)MeshMaterialSimple::Count)),
			};

			std::swap(newElem, tmpMesh.components[i]);
		}
		tmpMesh.posInfo = MeshPositionInfo{};

		pmr = newPmr;
	}

	DynamicMeshToken FinalizeTmp()
	{
		DynamicMeshToken ret{allBufs.size(), g_meshRendererInternal.frameNum};



		return ret;
	}

	void Destroy()
	{
		allBufs.clear();
		pmr = *std::pmr::null_memory_resource();
	}
};

/*
* A MeshComponent is a single element of a mesh unit for statics OR dynamics. The renderer creates a list of these
* and sorts so that consecutive elements of certain intervals are elligable for fusing (the spaceship operator is
* used for this). Then it gives these intervals of dynamic meshes to the builder to actually fuse them. The
* builder only needs the vertData in this struct, but intervals are used elsewhere in the renderer which is why
* the other two fields exist.
*/
struct MeshComponent
{
	struct MeshUnitWrapper* unitWrapper;
	MeshVertData* vertData; // null for statics
	IMeshWrapper iMeshWrapper;

	std::weak_ordering operator<=>(const MeshComponent& rhs) const;
};

struct MeshBuilderInternal
{
	MbMeshBufs bufs;

	struct TmpMesh
	{
		// will always have at least MAX_SIMPLE_COMPONENTS
		std::vector<MeshVertData> components;
		size_t maxVerts, maxIndices;

		void Create(const MeshCreateFunc& createFunc, bool dynamic);
		void PackVertices();
		MeshPositionInfo CalcPosInfo();
	} tmpMesh;

	VectorStack<DynamicMeshUnit> dynamicMeshUnits;

	inline MeshVertData& GetSimpleMeshComponent(MeshPrimitiveType type, MeshMaterialSimple material)
	{
		return tmpMesh.components[SIMPLE_COMPONENT_INDEX(type, material)];
	}

	struct Fuser
	{
		/*
		* During rendering (or when creating static meshes), we'll be given an span via BeginIMeshCreation().
		* Consecutive elements are fused so long as they don't exceed the max vert/index count.
		*/

		// for keeping track of where we are in the given interval
		std::span<const MeshComponent> curSpan;
		// used for debug meshes
		std::span<const MeshComponent> lastFusedSpan;
		size_t maxVerts, maxIndices;
		bool dynamic;

		void BeginIMeshCreation(std::span<const MeshComponent> span, bool dynamic);
		IMeshWrapper GetNextIMeshWrapper();

	private:
		IMeshWrapper CreateIMeshFromSpan(std::span<const MeshComponent> span,
		                                 size_t totalVerts,
		                                 size_t totalIndices);

	} fuser;

	void FrameCleanup();
	const DynamicMeshUnit& GetDynamicMeshFromToken(DynamicMeshToken token) const;
};

inline MeshBuilderInternal g_meshBuilderInternal;

#endif
