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

// TODO rename to shape color
struct MeshColor
{
	const color32 faceColor, lineColor;

	static MeshColor Face(const color32& faceColor)
	{
		return MeshColor{faceColor, {0, 0, 0, 0}};
	}

	static MeshColor Wire(const color32& lineColor)
	{
		return MeshColor{{0, 0, 0, 0}, lineColor};
	}

	static MeshColor Outline(const color32& faceColor)
	{
		return MeshColor{faceColor, {faceColor.r, faceColor.g, faceColor.b, 255}};
	}
};

// very basic color lerp for very basic needs
inline color32 color32RgbLerp(color32 a, color32 b, float f)
{
	f = clamp(f, 0, 1);
	return {
	    RoundFloatToByte(a.r * (1 - f) + b.r * f),
	    RoundFloatToByte(a.g * (1 - f) + b.g * f),
	    RoundFloatToByte(a.b * (1 - f) + b.b * f),
	    RoundFloatToByte(a.a * (1 - f) + b.a * f),
	};
}

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
