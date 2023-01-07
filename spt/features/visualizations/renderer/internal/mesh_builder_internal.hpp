#pragma once

#include "internal_defs.hpp"
#include "vector_slice.hpp"
#include "materials_manager.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include <forward_list>

// a single vertex - may have position, color, uv coordinates, normal, etc
struct VertexData
{
	Vector pos;
	color32 col;
	float u, v;

	VertexData(){};
	VertexData(const Vector& pos, color32 color) : pos(pos), col(color) {}
	VertexData(const Vector& pos, color32 color, float u, float v) : pos(pos), col(color), u(u), v(v) {}
};

enum class MeshPrimitiveType
{
	Lines,
	Triangles,
	Count
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

// the intervals are: [first, second)
using MeshComponentContainer = std::vector<MeshComponent>;
using ComponentInterval = std::pair<MeshComponentContainer::iterator, MeshComponentContainer::iterator>;
using ConstComponentInterval =
    std::pair<MeshComponentContainer::const_iterator, MeshComponentContainer::const_iterator>;

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
	* For mesh fusing, the renderer will give us an interval of components from a list with BeginIMeshCreation().
	* Consecutive elements are fused so long as they don't exceed the max vert/index count. As we iterate we'll
	* update curIntrvl.first, and fusedIntrvl is the last interval used to create an IMesh (for debug meshes).
	*/

	struct
	{
		ConstComponentInterval curIntrvl;
		ConstComponentInterval fusedIntrvl;
		bool dynamic;
	} creationState;

	void BeginIMeshCreation(ConstComponentInterval intrvl, bool dynamic);
	IMeshWrapper _CreateIMeshFromInterval(ConstComponentInterval intrvl,
	                                      size_t totalVerts,
	                                      size_t totalIndices,
	                                      bool dynamic);
	IMeshWrapper GetNextIMeshWrapper();

	void FrameCleanup();
	const DynamicMeshUnit& GetDynamicMeshFromToken(DynamicMeshToken token) const;
};

inline MeshBuilderInternal g_meshBuilderInternal;

#define GET_VDATA_FACES_CUSTOM_MATERIAL(material) \
	g_meshBuilderInternal.GetComponentInCurrentMesh(MeshPrimitiveType::Triangles, material)

#define GET_VDATA_LINES_CUSTOM_MATERIAL(material) \
	g_meshBuilderInternal.GetComponentInCurrentMesh(MeshPrimitiveType::Lines, material)

#endif
