#pragma once

#include "mesh_defs_public.hpp"
#include "spt\feature.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "mathlib\polyhedron.h"

// TODO have a struct for color & ztest for both face and line components

// clang-format off

/*
* The game uses a CMeshBuilder to create meshes, but we can't use it directly because parts of its implementation
* are private and/or not in the SDK. Not to worry - Introducing The MeshBuilderPro™! The MeshBuilderPro™ can be
* used to create meshes made of both faces and lines as well as different colors. The MeshBuilderPro™ also reuses
* internal mesh & vertex buffers for speed™ and efficiency™.
* 
* You can create two kinds of meshes: static and dynamic. Both kinds are made with a MeshCreateFunc which gives
* you a mesh builder delegate that you can use to create your mesh (trying to create a mesh outside of a
* MeshCreateFunc func is undefined behavior). There is no way to edit meshes after creation except for a
* RenderCallback (see mesh_renderer.hpp).
* 
* For stuff that changes a lot or used in e.g. animations, you'll probably want to use dynamic meshes. These must
* be created in the MeshRenderer signal (doing so anywhere else is undefined behavior), and their cleanup is
* handle automatically.
* 
* Static meshes are for stuff that rarely changes and/or stuff that's expensive to recreate every frame. These
* guys are more efficient and can be created at any time (in LoadFeature() or after). To render them you simply
* give your static mesh to the meshrenderer on whatever frames you want it to be drawn. Before rendering, you must
* call their Valid() method to check if they're alive, and recreate them if not. Internally they are shared
* pointers, so they'll get deleted once you get rid of the last copy.
* 
* Creating few big meshes is MUCH more efficient than many small ones, and this is great for opaque meshes. If
* your mesh has ANY translucent vertex however, the whole thing is considered translucent, and it's possible you
* may get meshes getting rendered in the wrong order relative to each other. For translucent meshes the best way
* to solve this is to break your mesh apart into small pieces, but this sucks for fps :(.
*/
class MeshBuilderDelegate
{
public:
	// a single line segment
	void AddLine(const Vector& v1, const Vector& v2, color32 c, bool zTest = true);

	// points is a pair-wise array of separate line segments
	void AddLines(const Vector* points, int nSegments, color32 c, bool zTest = true);

	// points is an array of points connected by lines
	void AddLineStrip(const Vector* points, int nPoints, bool loop, color32 c, bool zTest = true);

	// simple position indicator
	void AddCross(const Vector& pos, float radius, color32 c, bool zTest = true);

	void AddTri(const Vector& v1, const Vector& v2, const Vector& v3, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	// verts is a 3-pair-wise array of points
	void AddTris(const Vector* verts, int nFaces, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	void AddQuad(const Vector& v1, const Vector& v2, const Vector& v3, const Vector& v4, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	// verts is a 4-pair-wise array of points
	void AddQuads(const Vector* verts, int nFaces, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	// verts is a array of points, polygon is assumed to be simple & convex
	void AddPolygon(const Vector* verts, int nVerts, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	// 'pos' is the circle center, an 'ang' of <0,0,0> means the circle normal points towards x+
	void AddCircle(const Vector& pos, const QAngle& ang, float radius, int nPoints, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	void AddEllipse(const Vector& pos, const QAngle& ang, float radiusA, float radiusB, int nPoints, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	void AddBox(const Vector& pos, const Vector& mins, const Vector& maxs, const QAngle& ang, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	// nSubdivisions >= 0, 0 subdivsions is just a cube :)
	void AddSphere(const Vector& pos, float radius, int nSubdivisions, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	void AddSweptBox(const Vector& start, const Vector& end, const Vector& mins, const Vector& maxs, MeshColor mcStart, MeshColor mcEnd, MeshColor mcSweep, bool zTest = true, WindingDir wd = WD_CW);

	// 'pos' is at the center of the cone base, an 'ang' of <0,0,0> means the cone tip will point towards x+
	void AddCone(const Vector& pos, const QAngle& ang, float height, float radius, int nCirclePoints, bool drawBase, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	// 'pos' is at the center of the base, an ang of <0,0,0> means the normals are facing x+/x- for top/base
	void AddCylinder(const Vector& pos, const QAngle& ang, float height, float radius, int nCirclePoints, bool drawCap1, bool drawCap2, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	// a 3D arrow with its tail base at 'pos' pointing towards 'target'
	void AddArrow3D(const Vector& pos, const Vector& target, float tailLength, float tailRadius, float tipHeight, float tipRadius, int nCirclePoints, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

	void AddCPolyhedron(const CPolyhedron* polyhedron, MeshColor mc, bool zTest = true, WindingDir wd = WD_CW);

private:
	MeshBuilderDelegate() = default;
	MeshBuilderDelegate(MeshBuilderDelegate&) = delete;

	friend class MeshBuilderPro;

	// internal construction helper methods

	void _AddLine(const Vector& v1, const Vector& v2, color32 c, struct MeshVertData& vd);
	void _AddLineStrip(const Vector* points, int nPoints, bool loop, color32 c, struct MeshVertData& vd);
	void _AddPolygon(const Vector* verts, int nVerts, MeshColor mc, WindingDir wd, struct MeshVertData& vdf, struct MeshVertData& vdl);
	void _AddFaceTriangleStripIndices(struct MeshVertData& vdf, size_t vIdx1, size_t vIdx2, size_t nVerts, bool loop, bool mirror, WindingDir wd);
	void _AddFacePolygonIndices(struct MeshVertData& vdf, size_t vertsIdx, int nVerts, WindingDir wd);
	void _AddLineStripIndices(struct MeshVertData& vdl, size_t vertsIdx, int nVerts, bool loop);
	void _AddSubdivCube(int nSubdivisions, MeshColor mc, WindingDir wd, struct MeshVertData& vdf, struct MeshVertData& vdl);
	Vector* _CreateEllipseVerts(const Vector& pos, const QAngle& ang, float radiusA, float radiusB, int nPoints);
};

// clang-format on

typedef std::function<void(MeshBuilderDelegate& mb)> MeshCreateFunc;

// give this guy a function which accepts a mesh builder delegate -
// it'll be executed immediately so you can capture stuff by reference if you're using a lambda
class MeshBuilderPro
{
public:
	DynamicMesh CreateDynamicMesh(const MeshCreateFunc& createFunc);
	StaticMesh CreateStaticMesh(const MeshCreateFunc& createFunc);
};

inline MeshBuilderPro spt_meshBuilder;

#define MB_DYNAMIC(func) spt_meshBuilder.CreateDynamicMesh([&](MeshBuilderDelegate& mb) { func })
#define MB_STATIC(func) spt_meshBuilder.CreateStaticMesh([&](MeshBuilderDelegate& mb) { func })

#endif

class CPhysCollide;
class IPhysicsObject;
class CBaseEntity;

// collide stuff - returned mesh is an array of tris, consider caching and/or using static meshes
class CreateCollideFeature : public FeatureWrapper<CreateCollideFeature>
{
public:
	// the verts don't contain pos/rot info, so they'll be at the origin
	std::unique_ptr<Vector> CreateCollideMesh(const CPhysCollide* pCollide, int& outNumTris);

	// you'll need to transform the verts by applying a matrix you can create with pPhysObj->GetPosition()
	std::unique_ptr<Vector> CreatePhysObjMesh(const IPhysicsObject* pPhysObj, int& outNumTris);

	// you'll need to transform the verts like described above
	std::unique_ptr<Vector> CreateEntMesh(const CBaseEntity* pEnt, int& outNumTris);

	// can be used after InitHooks()
	static IPhysicsObject* GetPhysObj(const CBaseEntity* pEnt);

	// can be used (after InitHooks()) to check if the above functions do anything
	bool Works();

protected:
	void InitHooks() override;
	void UnloadFeature() override;

private:
	inline static bool cachedOffset = false;
	inline static int cached_phys_obj_off;

	DECL_MEMBER_THISCALL(int, CPhysicsCollision__CreateDebugMesh, const CPhysCollide* pCollide, Vector** outVerts);
};

inline CreateCollideFeature spt_collideToMesh;
