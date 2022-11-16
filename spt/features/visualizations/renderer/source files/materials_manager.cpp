#include "stdafx.h"

#include "..\mesh_defs_private.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "interfaces.hpp"

void PenisRegenerator::RegenerateTextureBits(ITexture* pTexture, IVTFTexture* pVTFTexture, Rect_t* pRect)
{
	if (pVTFTexture->Format() != IMAGE_FORMAT_BGRA8888)
		Error("spt: BAD!");
	memcpy(pVTFTexture->ImageData(), g_meshMaterialMgr.texBuf, sizeof g_meshMaterialMgr.texBuf);
}

void MeshBuilderMatMgr::Load()
{
	KeyValues* kv;

	kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	matOpaque = interfaces::materialSystem->CreateMaterial("_spt_UnlitOpaque", kv);

	/*kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	matAlpha = interfaces::materialSystem->CreateMaterial("_spt_UnlitTranslucent", kv);*/

	char texName[20];
	snprintf(texName, sizeof texName, "penis_%d", (int)time(nullptr));

	tex = interfaces::materialSystem->CreateProceduralTexture(texName,
	                                                          SPT_TEXTURE_GROUP,
	                                                          FONT_PAGE_SIZE,
	                                                          FONT_PAGE_SIZE,
	                                                          IMAGE_FORMAT_BGRA8888,
	                                                          TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT
	                                                              | TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD
	                                                              | TEXTUREFLAGS_SINGLECOPY);

	for (int x = 0; x < FONT_PAGE_SIZE; x++)
	{
		for (int y = 0; y < FONT_PAGE_SIZE; y++)
		{
			int i = y * 512 + x;
			texBuf[i * 4 + 0] = x / 2;
			texBuf[i * 4 + 1] = y / 2;
			texBuf[i * 4 + 2] = 255;
			texBuf[i * 4 + 3] = 255;
		}
	}

	// tex->SetTextureRegenerator(&regen);
	// EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE???????
	typedef void(__thiscall * _SetTextureRegenerator)(void* thisptr, ITextureRegenerator*);
	((_SetTextureRegenerator**)tex)[0][0x30 / 4](tex, &regen);

	// tex->Download();
	// EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE
	typedef void(__thiscall * _Download)(void* thisptr, Rect_t*, int);
	((_Download**)tex)[0][0x34 / 4](tex, nullptr, 0);

	kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	kv->SetString("$basetexture", texName);

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
	if (tex)
		tex->DecrementReferenceCount();
	matOpaque = matAlpha = matAlphaNoZ = nullptr;
}

IMaterial* MeshBuilderMatMgr::GetMaterial(bool opaque, bool zTest, color32 colorMod)
{
	if (!materialsInitialized || colorMod.a == 0)
		return nullptr;
	IMaterial* mat = zTest ? (opaque && colorMod.a ? matOpaque : matAlpha) : matAlphaNoZ;
	Assert(mat);
	mat->ColorModulate(colorMod.r / 255.f, colorMod.g / 255.f, colorMod.b / 255.f);
	mat->AlphaModulate(colorMod.a / 255.f);
	return mat;
}

#endif
