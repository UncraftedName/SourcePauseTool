#pragma once

#include "../tr_structs.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#define TR_IMG_VEC_FMT "<%f, %f, %f>"
#define TR_IMG_VEC_UNP(v) (v).x, (v).y, (v).z

#include "../tr_multiple_player.hpp"
#include "../tr_render_cache.hpp"
#include "../tr_record_cache.hpp"

#include "thirdparty/imgui/ImGuiFileDialog/ImGuiFileDialog.h"

namespace player_trace::tr_imgui
{
	struct ImGuiDetailedInfoTraceSelection
	{
		TrTracePlayer* tp;
		tr_tick activeTick;
	};

	std::string TrGetDisplayPath(const std::filesystem::path& absPath);

	void TraceFileSelectionTabCallback(TrTracePlayer& tfm, std::unique_ptr<ImGuiFileDialog>& igfd);
	void TraceFileSelectionWindowCallback(TrTracePlayer& tfm, std::unique_ptr<ImGuiFileDialog>& igfd);

	bool DrawDetailedTraceSelect(ImGuiDetailedInfoTraceSelection& info);
	void DrawDetailedInfoHeader(ImGuiDetailedInfoTraceSelection& info);

	void PlayerTabCallback(ImGuiDetailedInfoTraceSelection& info);
	void EntityTabCallback(ImGuiDetailedInfoTraceSelection& info);
	void PortalTabCallback(ImGuiDetailedInfoTraceSelection& info);

	void RenderStyleTab(TrTracePlayer& tracePlayer);
} // namespace player_trace::tr_imgui

#endif