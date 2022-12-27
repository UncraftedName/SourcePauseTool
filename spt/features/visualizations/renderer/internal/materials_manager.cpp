#include "stdafx.h"

#include "internal_defs.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "interfaces.hpp"

void MeshBuilderMatMgr::Load()
{
	KeyValues* kv;

	kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	matOpaque = interfaces::materialSystem->CreateMaterial("_spt_UnlitOpaque", kv);

	kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	matAlpha = interfaces::materialSystem->CreateMaterial("_spt_UnlitTranslucent", kv);

	kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	kv->SetInt("$ignorez", 1);
	matAlphaNoZ = interfaces::materialSystem->CreateMaterial("_spt_UnlitTranslucentNoZ", kv);

	materialsInitialized = matOpaque && matAlpha && matAlphaNoZ;
	if (!materialsInitialized)
		matOpaque = matAlpha = matAlphaNoZ = nullptr;
}

void MeshBuilderMatMgr::Unload()
{
	materialsInitialized = false;
	if (matOpaque)
		matOpaque->DecrementReferenceCount();
	if (matAlpha)
		matAlpha->DecrementReferenceCount();
	if (matAlphaNoZ)
		matAlphaNoZ->DecrementReferenceCount();
	interfaces::materialSystem->UncacheUnusedMaterials();
	matOpaque = matAlpha = matAlphaNoZ = nullptr;
}

#endif
