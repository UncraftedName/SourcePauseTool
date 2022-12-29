#include "stdafx.h"

#include "..\mesh_builder.hpp"
#include "spt\features\ent_props.hpp"
#include "spt\utils\interfaces.hpp"

std::unique_ptr<Vector> CreateCollideFeature::CreateCollideMesh(const CPhysCollide* pCollide, int& outNumTris)
{
	if (!pCollide)
	{
		outNumTris = 0;
		return nullptr;
	}
	Vector* outVerts;
	outNumTris = interfaces::physicsCollision->CreateDebugMesh(pCollide, &outVerts) / 3;
	return std::unique_ptr<Vector>(outVerts);
}

std::unique_ptr<Vector> CreateCollideFeature::CreatePhysObjMesh(const IPhysicsObject* pPhysObj, int& outNumTris)
{
	if (!pPhysObj)
	{
		outNumTris = 0;
		return nullptr;
	}
	return CreateCollideMesh(pPhysObj->GetCollide(), outNumTris);
}

std::unique_ptr<Vector> CreateCollideFeature::CreateEntMesh(const CBaseEntity* pEnt, int& outNumTris)
{
	IPhysicsObject* pPhysObj = GetPhysObj(pEnt);
	if (!pPhysObj)
	{
		outNumTris = 0;
		return nullptr;
	}
	return CreatePhysObjMesh(pPhysObj, outNumTris);
}

IPhysicsObject* CreateCollideFeature::GetPhysObj(const CBaseEntity* pEnt)
{
	if (!cachedOffset)
	{
		cached_phys_obj_off = spt_entprops.GetFieldOffset("CBaseEntity", "m_CollisionGroup", true);
		if (cached_phys_obj_off != utils::INVALID_DATAMAP_OFFSET)
			cached_phys_obj_off = (cached_phys_obj_off + 4) / 4;
		cachedOffset = true;
	}
	if (cached_phys_obj_off == utils::INVALID_DATAMAP_OFFSET)
		return nullptr;
	return *((IPhysicsObject**)pEnt + cached_phys_obj_off);
}
