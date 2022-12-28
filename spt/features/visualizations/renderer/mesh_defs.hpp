#pragma once

#include "spt\features\overlay.hpp"

#if !defined(OE) && defined(SPT_OVERLAY_ENABLED)

#define SPT_MESH_RENDERING_ENABLED

struct StaticMeshUnit;

// these are limits imposed by the game
#define MAX_MESH_VERTS 32767
#define MAX_MESH_INDICES 32767

// used internally for figuring out the render order of translucent meshes, but is also passed to callbacks
struct MeshPositionInfo
{
	Vector mins, maxs;
};

// winding direction, has no effect on lines
enum WindingDir
{
	WD_CW = 1,       // clockwise
	WD_CCW = 1 << 1, // counter clockwise
	WD_BOTH = WD_CW | WD_CCW
};

// a wrapper of a shared static mesh pointer, before rendering check if it's Valid() and if not recreate it
class StaticMesh
{
public:
	std::shared_ptr<StaticMeshUnit> meshPtr;

private:
	// keep a linked list through all static meshes for proper cleanup w/ tas_restart
	StaticMesh *prev, *next;
	static StaticMesh* first;

	void AttachToFront();

public:
	StaticMesh();
	StaticMesh(const StaticMesh& other);
	StaticMesh(StaticMesh&& other);
	StaticMesh(StaticMeshUnit* mesh);
	StaticMesh& operator=(const StaticMesh& r);
	bool Valid() const;
	void Destroy();
	static void DestroyAll();
	~StaticMesh();
};

// a token representing a dynamic mesh
struct DynamicMesh
{
	const size_t dynamicMeshIdx;
	const int createdFrame;
};

#endif
