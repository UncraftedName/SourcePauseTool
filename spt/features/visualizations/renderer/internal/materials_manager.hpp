#pragma once

#include "internal_defs.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

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

struct MeshBuilderMatMgr
{
	MaterialRef matOpaque, matAlpha, matAlphaNoZ;

	void Load();
	void Unload();
};

inline MeshBuilderMatMgr g_meshMaterialMgr;

#endif
