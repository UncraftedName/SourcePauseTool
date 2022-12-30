/*
* This file functions as the header for the internal mesh builder and material manager, and also contains
* constants/typedefs used by the whole rendering system. Like the mesh renderer, there are 3 mesh builders:
* - MeshBuilderPro (spt_meshBuilder)
* - MeshBuilderDelegate
* - MeshBuilderInternal (g_meshBuilderInternal)
* The MeshBuilderPro is stateless, and will give the user a stateless delegate when they want to create a mesh.
* The user then calls the various methods of the delegate which will edit the state of the internal builder.
* 
* At the end of the day, the mesh builder's purpose is to create IMesh* objects so that the renderer can call
* IMesh->draw(). For static meshes, this is simple enough - the user can fill up the builder's buffers with data,
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
* for an IMesh*, you provide not just 1 but a whole range of meshes, then the builder will automatically fuse
* consecutive elements and give you one IMesh* object at a time until it's done iterating over that range.
*/

#pragma once

#include "..\mesh_defs.hpp"

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
#include <forward_list>

#include "spt\utils\interfaces.hpp"
#include "vector_slice.hpp"

// used to check if dynamic mesh tokens are valid, increases every frame
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

struct MaterialRef
{
	MaterialRef();
	MaterialRef(IMaterial* mat);
	MaterialRef(const MaterialRef& other);
	MaterialRef(MaterialRef&& other);
	MaterialRef& operator=(const MaterialRef& other);
	MaterialRef& operator=(IMaterial* mat);
	bool operator==(const MaterialRef& other) const;
	auto operator<=>(const MaterialRef& other) const;
	operator IMaterial*() const;
	IMaterial* operator->() const;
	~MaterialRef();

private:
	IMaterial* mat;
};

// contains the instructions for creating an IMesh* object
struct MeshVertData
{
	VectorSlice<VertexData> verts;
	VectorSlice<VertIndex> indices;
	MeshPrimitiveType type;
	MaterialRef material;

	MeshVertData() = default;

	MeshVertData(MeshVertData&& other);

	MeshVertData(std::vector<VertexData>& vertDataVec,
	             std::vector<VertIndex>& vertIndexVec,
	             MeshPrimitiveType type,
	             MaterialRef material);

	MeshVertData& operator=(MeshVertData&& other);

	bool Empty() const;
};

// does not handle destruction if the imesh
struct IMeshWrapper
{
	IMesh* iMesh;
	MeshPrimitiveType type;
	MaterialRef material;
};

/*
* Mesh units are collections of meshes. Each IMesh* object can only have one material and one primitive type. But
* as a user I want to say "this mesh should have lines AND triangles". Dynamic mesh units will have a list of
* MeshVertData which can be used to build IMesh* objects during rendering. Static meshes will just have a list of
* IMesh* objects and their corresponding materials. Both static and dynamic meshes also need a position metric
* which is used for attempting to render translucent meshes in order.
*/

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

/*
* A mesh component is what I call each element of a mesh unit. The renderer will create a list of these and sort
* them, then feed slices of that list to the builder to create fused IMesh* objects. The builder can only ever
* accept dynamic meshes to fuse, (it never uses either of the wrapper objects); but it just ended up being more
* convenient to give the builder a list of MeshComponents than creating an array of MeshVertData from that. The
* spaceship operator is used by the renderer for ordering the components.
*/

struct MeshComponent
{
	struct MeshUnitWrapper* unitWrapper;
	MeshVertData* vertData; // null for statics
	IMeshWrapper iMeshWrapper;

	std::weak_ordering operator<=>(const MeshComponent& rhs) const;
};

/*
* When a mesh is being created, its data is stored in curMeshVertData. Dynamic meshes then get moved to the shared
* list, but statics are put on the heap. Each MeshVertData is simply a slice of the shared vertex/index lists. The
* reason we need a list of shared arrays is because each mesh can have multiple components. As a mesh is created,
* it will request MeshVertData from curMeshVertData that has a specific material and primitive type by calling
* GetComponentInCurrentMesh(). If one is not found, a new one is created that has slices of the end of the Nth
* shared list (for both verts & indices).
* 
* For example, say we have 5 dynamic meshes and each one has [1, 3, 1, 2, 3] components respectively. The first
* mesh will only use a slice of the first shared list, the second will use a slice of the first 3, etc. The shared
* vertex lists will look like this:
* 
* sharedVertLists[0]: comp_slice[0,0]..comp_slice[1,0]..comp_slice[2,0]..comp_slice[3,0]..comp_slice[4,0]
* sharedVertLists[1]: comp_slice[1,1]..comp_slice[3,1]
* sharedVertLists[2]: comp_slice[1,2]
* 
* The shared index lists will look exactly the same, and the shared MeshVertData list will look like this:
* 
* sharedVertDataArrays: meshVertData[0]..meshVertData[1]....meshVertData[2]..meshVertData[3]....meshVertData[4]
*                       V                V                  V                V                  V
*                       comp_slice[0,0]..comp_slice[1,0:3]..comp_slice[2,0]..comp_slice[3,0:2]..comp_slice[4,0:3]
* 
* We have to use linked lists intead of vectors because each slice keeps a pointer to the vector and allocations
* would move it around if we used a vector of vectors. This system supports arbitrary many components, but adding
* anything to a mesh unit requires an O(N) lookup for N components. In practice this doesn't matter, for basic
* geometry meshes N<=6 and usually N<=2. Also, since the shared lists are chosen in order, the first one will
* always end up being the biggest.
*/

// the ranges are: [first, second)
using MeshComponentContainer = std::vector<MeshComponent>;
using ComponentRange = std::pair<MeshComponentContainer::iterator, MeshComponentContainer::iterator>;
using ConstComponentRange = std::pair<MeshComponentContainer::const_iterator, MeshComponentContainer::const_iterator>;

// TODO add functionality to convert to collides
struct MeshBuilderInternal
{
	struct
	{
		// can't use std::swap on the inner vectors since they're pointed to from slices
		std::forward_list<std::vector<VertexData>> verts;
		std::forward_list<std::vector<VertIndex>> indices;
		std::vector<MeshVertData> vertData;
	} sharedLists;

	VectorSlice<MeshVertData> curMeshVertData;

	VectorStack<DynamicMeshUnit> dynamicMeshUnits;

	MeshVertData& GetComponentInCurrentMesh(MeshPrimitiveType type, MaterialRef material);
	MeshPositionInfo _CalcPosInfoForCurrentMesh();

	/*
	* For mesh fusing, the renderer will give us a range of components from a list with BeginIMeshCreation().
	* Consecutive elements are fused so long as they don't exceed the max vert/index count. As we iterate we'll
	* update curRange.first, and fusedRange will have the last range used to create an IMesh (for debug meshes).
	*/

	struct
	{
		ConstComponentRange curRange;
		ConstComponentRange fusedRange;
		bool dynamic;
	} creationState;

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
	MaterialRef matOpaque = nullptr, matAlpha = nullptr, matAlphaNoZ = nullptr;

	void Load();
	void Unload();
};

inline MeshBuilderMatMgr g_meshMaterialMgr;

#endif
