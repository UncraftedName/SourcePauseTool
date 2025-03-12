#include <stdafx.hpp>

#include "ent_list.hpp"
#include "spt\features\ent_props.hpp"
#include "interfaces.hpp"

#include "server_class.h"

template<>
void utils::SptEntList<true>::RebuildLists()
{
	if (!interfaces::engine_tool)
		return;
	int newTick = interfaces::engine_tool->HostTick();
	if (newTick == lastUpdatedAt)
		return;
	lastUpdatedAt = newTick;
	if (!interfaces::engine_server)
		return;
	_entList.clear();
	_portalList.clear();
	if (!Valid())
		return;
	int maxNEnts = interfaces::engine_server->GetEntityCount();
	for (int i = 0; (int)_entList.size() < maxNEnts && i < MAX_EDICTS; i++)
	{
		IServerEntity* ent = GetEnt(i);
		if (!ent)
			continue;
		_entList.push_back(ent);
		if (EntIsPortal(ent))
		{
			_portalList.emplace_back();
			FillPortalInfo(ent, _portalList.back());
		}
	}
}

template<>
auto utils::SptEntList<true>::GetEnt(int i) -> ent_type
{
	if (!interfaces::engine_server)
		return nullptr;
	edict_t* ed = interfaces::engine_server->PEntityOfEntIndex(i);
	if (!ed || (int)ed == -1) // -1 check is from ent_props, not sure if it's necessary
		return nullptr;
	return ed->GetIServerEntity();
}

template<>
const char* utils::SptEntList<true>::EntClassName(ent_type ent)
{
	constexpr int fOff = -(int)sizeof(CBaseHandle);
	static CachedField<ServerClass*, "CBaseEntity", "m_Network.m_hParent", true, false, fOff> fClass;
	return fClass.Exists() && *fClass.GetPtr(ent) ? (**fClass.GetPtr(ent)).GetName() : nullptr;
}

template<>
void utils::SptEntList<true>::FillPortalInfo(ent_type ent, utils::PortalInfo& info)
{
	Assert(EntIsPortal(ent));
	static utils::CachedField<Vector, "CProp_Portal", "m_vecAbsOrigin", true> fPos;
	static utils::CachedField<QAngle, "CProp_Portal", "m_angAbsRotation", true> fAng;
	static utils::CachedField<CBaseHandle, "CProp_Portal", "m_hLinkedPortal", true> fLinked;
	static utils::CachedField<bool, "CProp_Portal", "m_bIsPortal2", true> fIsPortal2;
	static utils::CachedField<bool, "CProp_Portal", "m_bActivated", true> fActivated;
	static utils::CachedFields portalFields{fPos, fAng, fLinked, fIsPortal2, fActivated};

	if (!portalFields.HasAll())
	{
		Assert(0);
		return;
	}
	auto [pos, ang, linked, p2, activated] = portalFields.GetAllPtrs(ent);
	auto linkedEnt = linked->IsValid() ? GetEnt(linked->GetEntryIndex()) : nullptr;

	info.handle = ent->GetRefEHandle();
	info.linkedHandle = *linked;
	info.pos = *pos;
	info.linkedPos = linkedEnt ? *fPos.GetPtr(linkedEnt) : Vector{NAN};
	info.ang = *ang;
	info.linkedAng = linkedEnt ? *fAng.GetPtr(linkedEnt) : QAngle{NAN, NAN, NAN};
	info.isOrange = *p2;
	info.isActivated = *activated;
	info.isOpen = linked->IsValid();
}

template<>
const utils::PortalInfo* utils::SptEntList<true>::GetEnvironmentPortal()
{
	static utils::CachedField<CBaseHandle, "CPortal_Player", "m_hPortalEnvironment", true> fEnv;
	CBaseHandle* handle = fEnv.GetPtrPlayer();
	return handle && handle->IsValid() ? GetPortalAtIndex(handle->GetEntryIndex()) : nullptr;
}
