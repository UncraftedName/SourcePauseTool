#pragma once

#include <vector>
#include <algorithm>

#include "iserverentity.h"
#include "icliententity.h"
#include "basehandle.h"

namespace utils
{
	struct PortalInfo
	{
		CBaseHandle handle, linkedHandle;
		Vector pos, linkedPos;
		QAngle ang, linkedAng;
		bool isOrange, isActivated, isOpen;

		inline const PortalInfo* GetLinkedPortal() const;

		void Invalidate()
		{
			handle = linkedHandle = CBaseHandle{};
			pos = linkedPos = Vector{NAN};
			ang = linkedAng = QAngle{NAN, NAN, NAN};
		}

		bool operator==(const PortalInfo& other) const
		{
			return !memcmp(this, &other, sizeof other);
		}

		bool operator!=(const PortalInfo& other) const
		{
			return !!memcmp(this, &other, sizeof other);
		}
	};

	/*
	* A server/client entity list abstraction. Stores an internal list for all entities which is
	* rebuilt every tick/frame for server/client. Also does the same for portals. All portal info
	* is returned by POINTERS which should NOT be stored inside a feature. All pointers are invalid
	* on the next tick/frame. If required, portal info can be copied and stored by value, but this
	* should not be necessary for most features.
	*/
	template<bool SERVER>
	class SptEntList
	{
		using ent_type = std::conditional_t<SERVER, IServerEntity*, IClientEntity*>;

	private:
		std::vector<ent_type> _entList;
		std::vector<PortalInfo> _portalList;
		int lastUpdatedAt = -666;

		void RebuildLists();
		void FillPortalInfo(ent_type ent, PortalInfo& info);

	public:
		SptEntList() = default;
		SptEntList(SptEntList&) = delete;
		SptEntList(SptEntList&&) = delete;

		const auto& GetEntList()
		{
			RebuildLists();
			return _entList;
		}

		const auto& GetPortalList()
		{
			RebuildLists();
			return _portalList;
		}

		static ent_type GetPlayer()
		{
			return GetEnt(1);
		};

		static bool EntIsPortal(ent_type ent)
		{
			const char* name = EntClassName(ent);
			return name && !strcmp(name, "CProp_Portal");
		}

		const PortalInfo* GetPortalAtIndex(int index)
		{
			auto& portalList = GetPortalList();
			auto it = std::lower_bound(portalList.cbegin(),
			                           portalList.cend(),
			                           index,
			                           [](const PortalInfo& portal, int x)
			                           { return portal.handle.GetEntryIndex() < x; });
			return it != portalList.cend() && it->handle.GetEntryIndex() == index ? &*it : nullptr;
		}

		static bool Valid()
		{
			return !!GetEnt(0);
		}

		const PortalInfo* GetEnvironmentPortal();
		static ent_type GetEnt(int index);
		static const char* EntClassName(ent_type ent);
	};

	// the singleton objects
	inline SptEntList<true> spt_serverEntList;
	inline SptEntList<false> spt_clientEntList;

	/*
	* Portal wrappers that can be used directly from the 'utils' namespace. The portal info struct
	* is server/client agnostic by design with some slight differences:
	* - the server position is more accurate
	* - entity handle serial numbers are different (TODO check)
	* 
	* This logic prefers getting portal info from the server when available, otherwise gets it from
	* the client.
	*/

	// avoid copying by value :p
	static auto& GetPortalList()
	{
		return spt_serverEntList.Valid() ? spt_serverEntList.GetPortalList()
		                                 : spt_clientEntList.GetPortalList();
	}

	static auto GetEnvironmentPortal()
	{
		return spt_serverEntList.Valid() ? spt_serverEntList.GetEnvironmentPortal()
		                                 : spt_clientEntList.GetEnvironmentPortal();
	}

	static const PortalInfo* GetPortalAtIndex(int index)
	{
		return spt_serverEntList.Valid() ? spt_serverEntList.GetPortalAtIndex(index)
		                                 : spt_clientEntList.GetPortalAtIndex(index);
	}

	inline const PortalInfo* PortalInfo::GetLinkedPortal() const
	{
		return handle.IsValid() && linkedHandle.IsValid() ? GetPortalAtIndex(linkedHandle.GetEntryIndex())
		                                                  : nullptr;
	}

} // namespace utils
