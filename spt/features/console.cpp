#include "stdafx.hpp"
#include "..\feature.hpp"

#include "vgui_controls\consoledialog.h"
#include "vgui_controls\RichText.h"

/*
* - submodules need small changes for these^ imports to work
* - this crashes - GetPanel() returns garbage, maybe try on 3420?
*/
CON_COMMAND_F(un_print_stream_end, "command <x chars>", FCVAR_DONTRECORD)
{
	if (args.ArgC() < 2)
	{
		Warning("enter a number of characters to print\n");
		return;
	}

	int numChars = strtol(args.Arg(1), nullptr, 10);

	struct RichText_guts : public vgui::RichText
	{
		CUtlVector<wchar_t>& GetTextStream()
		{
			return *(CUtlVector<wchar_t>*)((uint32_t)&_vertScrollBar + 4);
		}
	};

	struct CConsolePanel_guts : public vgui::CConsolePanel
	{
		RichText_guts* GetHistory()
		{
			return (RichText_guts*)m_pHistory;
		}
	};

	struct CConsoleDialog_guts : public vgui::CConsoleDialog
	{
		CConsolePanel_guts* GetPanel()
		{
			return (CConsolePanel_guts*)m_pConsolePanel;
		}
	};

	struct CGameConsole_guts
	{
		void** vt;
		bool m_bInitialized;
		CConsoleDialog_guts* m_pConsole;
	};

	uint32_t engine;
	MemUtils::GetModuleInfo(L"engine.dll", nullptr, (void**)&engine, nullptr);

	auto staticGameConsole = *(CGameConsole_guts**)(engine + 0x5b7c4c);
	auto& stream = staticGameConsole->m_pConsole->GetPanel()->GetHistory()->GetTextStream();
	std::stringstream ss;
	for (int i = stream.Size() - numChars; i < stream.Size(); i++)
	{
		ss << static_cast<char>(stream[i]);
	}
	std::string st = ss.str();
	Msg("%s\n", st.c_str());
}

class ConsoleFeature : public FeatureWrapper<ConsoleFeature>
{
public:
protected:
	virtual void LoadFeature() override
	{
		InitCommand(un_print_stream_end);
	};
};

static ConsoleFeature spt_console;
