#include "stdafx.h"

#include "internal_defs.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "interfaces.hpp"

/**************************************** MATERIAL REF ****************************************/

MaterialRef::MaterialRef() : mat(nullptr) {}

MaterialRef::MaterialRef(IMaterial* mat) : mat(mat)
{
	if (mat)
		mat->IncrementReferenceCount();
}

MaterialRef::MaterialRef(const MaterialRef& other) : MaterialRef(other.mat) {}

MaterialRef::MaterialRef(MaterialRef&& other) : mat(other.mat)
{
	other.mat = nullptr;
}

MaterialRef& MaterialRef::operator=(const MaterialRef& other)
{
	return *this = other.mat;
}

MaterialRef& MaterialRef::operator=(IMaterial* newMaterial)
{
	if (mat != newMaterial)
	{
		if (mat)
			mat->DecrementReferenceCount();
		if (newMaterial)
			newMaterial->IncrementReferenceCount();
	}
	mat = newMaterial;
	return *this;
}

bool MaterialRef::operator==(const MaterialRef& other) const
{
	return mat == other.mat;
}

auto MaterialRef::operator<=>(const MaterialRef& other) const
{
	return mat <=> other.mat;
}

MaterialRef::operator IMaterial*() const
{
	return mat;
}

IMaterial* MaterialRef::operator->() const
{
	return mat;
}

MaterialRef::~MaterialRef()
{
	*this = nullptr;
}

/**************************************** MATERIAL MANAGER ****************************************/

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
}

void MeshBuilderMatMgr::Unload()
{
	std::vector<IMaterial*> mats{matOpaque, matAlpha, matAlphaNoZ};
	for (IMaterial* mat : mats)
	{
		if (!mat)
			continue;
		mat->DecrementReferenceCount();
		mat->DeleteIfUnreferenced();
	}

	matOpaque = matAlpha = matAlphaNoZ = nullptr;
}

#endif
