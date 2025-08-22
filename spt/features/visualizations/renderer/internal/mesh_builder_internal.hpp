#pragma once

#include "internal_defs.hpp"
#include "materials_manager.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "spt/utils/interfaces.hpp"

#include <forward_list>
#include <memory_resource>
#include <span>
#include <variant>

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

struct IMeshWrapper
{
	IMesh* iMesh;
	MaterialRef material;
	bool dynamic;

	IMeshWrapper(IMesh* iMesh, MaterialRef material, bool dynamic)
	    : iMesh(iMesh), material(material), dynamic(dynamic)
	{
	}

	IMeshWrapper(IMeshWrapper&) = delete;

	IMeshWrapper(IMeshWrapper&& o) : iMesh(o.iMesh), material(std::move(o.material)), dynamic(dynamic)
	{
		o.iMesh = nullptr;
	}

	IMeshWrapper& operator=(IMeshWrapper&& other)
	{
		if (this != &other)
		{
			DestroyIMesh();
			iMesh = other.iMesh;
			other.iMesh = nullptr;
			material = std::move(other.material);
			dynamic = other.dynamic;
		}
		return *this;
	}

	void DestroyIMesh()
	{
		if (iMesh && !dynamic)
		{
			CMatRenderContextPtr context(interfaces::materialSystem);
			context->DestroyStaticMesh(iMesh);
		}
	}

	~IMeshWrapper()
	{
		DestroyIMesh();
	}
};

struct StaticMeshUnit
{
	const std::vector<IMeshWrapper> meshes;
	const MeshPositionInfo posInfo;

	StaticMeshUnit(std::vector<IMeshWrapper>&& meshes, const MeshPositionInfo& posInfo)
	    : meshes(std::move(meshes)), posInfo(posInfo)
	{
	}

	bool IsEmpty() const
	{
		return meshes.empty();
	}
};

struct MbComponentBufs
{
	std::pmr::vector<VertexData> verts;
	std::pmr::vector<VertIndex> indices;

	MeshPrimitiveType primType;
	MeshMaterialSimple matType;

	MbComponentBufs(std::pmr::memory_resource& mr, MeshPrimitiveType primType, MeshMaterialSimple matType)
	    : verts(&mr), indices(&mr), primType(primType), matType(matType)
	{
	}

	MaterialRef GetMaterial() const
	{
		return g_meshMaterialMgr.GetMaterial(matType);
	}

	bool IsEmpty() const
	{
		return indices.empty();
	}
};

struct MbStagingBufs
{
	std::array<MbComponentBufs, MAX_SIMPLE_COMPONENTS> components;
	size_t maxVerts, maxIndices;

	MbStagingBufs(std::pmr::memory_resource& mr, bool dynamic)
	    : components{
	          MbComponentBufs{mr, MeshPrimitiveType::Lines, MeshMaterialSimple::Opaque},
	          MbComponentBufs{mr, MeshPrimitiveType::Lines, MeshMaterialSimple::Alpha},
	          MbComponentBufs{mr, MeshPrimitiveType::Lines, MeshMaterialSimple::AlphaNoZ},
	          MbComponentBufs{mr, MeshPrimitiveType::Triangles, MeshMaterialSimple::Opaque},
	          MbComponentBufs{mr, MeshPrimitiveType::Triangles, MeshMaterialSimple::Alpha},
	          MbComponentBufs{mr, MeshPrimitiveType::Triangles, MeshMaterialSimple::AlphaNoZ},
	      }
	{
		GetMaxMeshSize(maxVerts, maxIndices, dynamic);
	}

	// TODO - make new components at runtime
	inline MbComponentBufs& GetSimpleMeshComponent(MeshPrimitiveType type, MeshMaterialSimple material)
	{
		return components[SIMPLE_COMPONENT_INDEX(type, material)];
	}

	void PackVertices(std::pmr::memory_resource& mr);
	MeshPositionInfo CalcPosInfo();
};

struct MbDynamicMeshUnit
{
	std::pmr::forward_list<MbComponentBufs> componentBufs;
	MeshPositionInfo posInfo;

	MbDynamicMeshUnit(std::pmr::forward_list<MbComponentBufs>&& componentBufs, const MeshPositionInfo& posInfo)
	    : componentBufs(std::move(componentBufs)), posInfo(posInfo)
	{
	}

	bool IsEmpty() const
	{
		return componentBufs.empty();
	}
};

struct MbComponent
{
	using dynamic_t = std::reference_wrapper<const MbComponentBufs>;
	using static_t = std::reference_wrapper<const IMeshWrapper>;

	std::variant<dynamic_t, static_t> component;

	MbComponent(const MbComponentBufs& dyn) : component(dyn) {}
	MbComponent(const IMeshWrapper& imw) : component(imw) {}

	bool IsDynamic() const
	{
		return std::holds_alternative<dynamic_t>(component);
	}

	constexpr const auto& GetDynamic() const
	{
		return std::get<dynamic_t>(component).get();
	}

	constexpr const auto& GetStatic() const
	{
		return std::get<static_t>(component).get();
	}

	MaterialRef GetMaterial() const
	{
		if (IsDynamic())
			return GetDynamic().GetMaterial();
		else
			return GetStatic().material;
	}
};

#endif
