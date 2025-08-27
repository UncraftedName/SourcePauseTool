#pragma once

#include "internal_defs.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "..\mesh_builder.hpp"
#include "materialsystem\itexture.h"

/*struct TextureRefMgr
{
	inline void AddRef(ITexture* tex) const
	{
		if (tex)
			tex->IncrementReferenceCount();
	}

	inline void Release(ITexture*& tex) const
	{
		if (tex)
			tex->DecrementReferenceCount(); // TODO delete if unreferenced
		tex = nullptr;
	}
};

using TextureRef = AutoRefPtr<ITexture*, TextureRefMgr>;*/

enum MbSimpleMeshMaterialType : unsigned char
{
	MB_SMMT_OPAQUE,
	MB_SMMT_ALPHA,
	MB_SMMT_ALPHA_NOZ,

	MB_SMMT_COUNT,
};

enum MbMeshPrimitiveType : unsigned char
{
	MB_MPT_LINES,
	MB_MPT_TRIS,

	MB_MPT_COUNT,
};

struct MbMaterialManager
{
	IMaterial *matOpaque, *matAlpha, *matAlphaNoZ;

	void Load();
	void Unload();

	IMaterial* GetMaterial(MbSimpleMeshMaterialType matType) const;
};

#endif
