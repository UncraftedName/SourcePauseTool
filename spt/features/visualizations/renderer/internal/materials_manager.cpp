#include "stdafx.hpp"

#include "internal_defs.hpp"
#include "materials_manager.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "interfaces.hpp"

/**************************************** MATERIAL MANAGER ****************************************/

void MbMaterialManager::Load()
{
	KeyValues* kv;

	kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	matOpaque = interfaces::materialSystem->CreateMaterial("_spt_UnlitOpaque", kv);
	matOpaque->IncrementReferenceCount();

	kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	matAlpha = interfaces::materialSystem->CreateMaterial("_spt_UnlitTranslucent", kv);
	matAlpha->IncrementReferenceCount();

	kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	kv->SetInt("$ignorez", 1);
	matAlphaNoZ = interfaces::materialSystem->CreateMaterial("_spt_UnlitTranslucentNoZ", kv);
	matAlphaNoZ->IncrementReferenceCount();
}

void MbMaterialManager::Unload()
{
	std::array<std::reference_wrapper<IMaterial*>, 3> mats{matOpaque, matAlpha, matAlphaNoZ};
	for (IMaterial* mat : mats)
	{
		// TODO - materials that have been used don't get deleted here, not an issue now but will be one with text
		if (mat)
		{
			mat->DecrementReferenceCount();
			mat->DeleteIfUnreferenced();
			mat = nullptr;
		}
	}
}

IMaterial* MbMaterialManager::GetMaterial(MbSimpleMeshMaterialType matType) const
{
	switch (matType)
	{
	case MB_SMMT_OPAQUE:
		return matOpaque;
	case MB_SMMT_ALPHA:
		return matAlpha;
	case MB_SMMT_ALPHA_NOZ:
		return matAlphaNoZ;
	default:
		Assert(0);
		return nullptr;
	}
}

#endif
