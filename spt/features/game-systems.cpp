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

	auto& gameSystems = *(CUtlVector<IGameSystem*>*)(serverBase + 0x65581c);
	const char pad[] = "............................................";
	for (int i = 0; i < gameSystems.Count(); i++)
	{
		IGameSystem* sys = gameSystems[i];
		if (sys)
		{
			Msg("%02d:%.*s%s, vtable at server.dll!0x%p\n",
			    i,
			    33 - strlen(sys->Name()),
			    pad,
			    sys->Name(),
			    *(uint32_t*)sys - serverBase);
		}
		else
		{
			Msg("%02d: NULL\n", i);
		}
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
