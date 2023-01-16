#include "stdafx.hpp"
#include "..\feature.hpp"

abstract_class IGameSystem
{
public:
	virtual char const* Name() = 0;
};

CON_COMMAND_F(un_show_game_systems, "shows all installed game systems", FCVAR_DONTRECORD)
{
	uint32_t serverBase;
	MemUtils::GetModuleInfo(L"server.dll", nullptr, (void**)&serverBase, nullptr);

	auto& s_GameSystems = *(CUtlVector<IGameSystem*>*)(serverBase + 0x65581c);
	auto& s_GameSystemsPerFrame = *(CUtlVector<IGameSystem*>*)(serverBase + 0x655830);

	size_t longestName = 0;
	for (int i = 0; i < s_GameSystems.Count(); i++)
	{
		if (!s_GameSystems[i])
			Error("spt: NULL game system at index %d", i);
		longestName = MAX(longestName, strlen(s_GameSystems[i]->Name()));
	}

	const size_t padBuf = 2;
	std::string pad(longestName + padBuf, '.');
	const char* cPad = pad.c_str();

	// the regular list (should be) a strict superset of the per frame list and in the same order
	for (int sysIdx = 0, sysPerFrameIdx = 0; sysIdx < s_GameSystems.Count(); sysIdx++)
	{
		IGameSystem* sys = s_GameSystems[sysIdx];
		const char* name = sys->Name();
		uint32_t vt = *(uint32_t*)sys - serverBase;
		size_t nPad = longestName + padBuf - strlen(name);

		if (sysPerFrameIdx < s_GameSystemsPerFrame.Count() && s_GameSystemsPerFrame[sysPerFrameIdx] == sys)
			Msg("%02d %02d:%.*s%s, vt: server.dll!0x%p\n", sysIdx, sysPerFrameIdx++, nPad, cPad, name, vt);
		else
			Msg("%02d **:%.*s%s, vt: server.dll!0x%p\n", sysIdx, nPad, pad.c_str(), name, vt);

		_sleep(0);
	}
}

class GameSystemFeature : public FeatureWrapper<GameSystemFeature>
{
protected:
	virtual void LoadFeature() override
	{
		InitCommand(un_show_game_systems);
	};
};

static GameSystemFeature spt_gamesystems;
