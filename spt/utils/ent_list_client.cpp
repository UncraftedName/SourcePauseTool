#include <stdafx.hpp>

#include "ent_list.hpp"
#include "spt/features/ent_props.hpp"
#include "interfaces.hpp"

template<>
void utils::SptEntList<false>::RebuildLists()
{
	if (!interfaces::engine_tool)
		return;
	int newFrame = interfaces::engine_tool->HostFrameCount();
	if (newFrame == lastUpdatedAt)
		return;
	lastUpdatedAt = newFrame;
	if (!interfaces::entListClient)
		return;
	_entList.clear();
	_portalList.clear();
	// we could just wrap this LL but it makes the code easier if we have the same structure for server/client
	for (const CEntInfo* info = interfaces::entListClient->FirstEntInfo(); info;
	     info = interfaces::entListClient->NextEntInfo(info))
	{
		if (!info->m_pEntity)
			continue;
		auto ent = static_cast<IClientEntity*>(info->m_pEntity);
		_entList.push_back(ent);
		if (EntIsPortal(ent))
		{
			_portalList.emplace_back();
			FillPortalInfo(ent, _portalList.back());
		}
	}
}

template<>
auto utils::SptEntList<false>::GetEnt(int i) -> ent_type
{
	// the engine does not check the upper bound for us :/
	if (!interfaces::entListClient || i >= NUM_ENT_ENTRIES)
		return nullptr;
	return static_cast<IClientEntity*>(interfaces::entListClient->LookupEntityByNetworkIndex(i));
}

template<>
const char* utils::SptEntList<false>::EntClassName(ent_type ent)
{
	return ent ? ent->GetClientClass()->GetName() : nullptr;
}

template<>
void utils::SptEntList<false>::FillPortalInfo(ent_type ent, utils::PortalInfo& info)
{
	Assert(EntIsPortal(ent));
	if (!interfaces::modelInfo)
	{
		Assert(0);
		return;
	}

	static utils::CachedField<CBaseHandle, "CProp_Portal", "m_hLinkedPortal", false> fLinked;
	static utils::CachedField<bool, "CProp_Portal", "m_bActivated", false> fActivated;
	if (!fLinked.Exists() || !fActivated.Exists())
	{
		Assert(0);
		return;
	}

	CBaseHandle linked = *fLinked.GetPtr(ent);
	auto linkedEnt = linked.IsValid() ? GetEnt(linked.GetEntryIndex()) : nullptr;

	info.handle = ent->GetRefEHandle();
	info.linkedHandle = *fLinked.GetPtr(ent);
	info.pos = ent->GetAbsOrigin();
	info.linkedPos = linkedEnt ? linkedEnt->GetAbsOrigin() : Vector{NAN};
	info.ang = ent->GetAbsAngles();
	info.linkedAng = linkedEnt ? linkedEnt->GetAbsAngles() : QAngle{NAN, NAN, NAN};
	info.isOrange = !!strstr(interfaces::modelInfo->GetModelName(ent->GetModel()), "portal2");
	info.isActivated = *fActivated.GetPtr(ent);
	info.isOpen = fLinked.GetPtr(ent)->IsValid();
}

template<>
const utils::PortalInfo* utils::SptEntList<false>::GetEnvironmentPortal()
{
	static utils::CachedField<CBaseHandle, "CPortal_Player", "m_hPortalEnvironment", false> fEnv;
	CBaseHandle* handle = fEnv.GetPtrPlayer();
	if (handle && handle->IsValid())
		return GetPortalAtIndex(handle->GetEntryIndex());
	return nullptr;
}
