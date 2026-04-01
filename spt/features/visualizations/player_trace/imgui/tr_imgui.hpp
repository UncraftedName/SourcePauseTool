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
		tr_tick activeTick;
	};

	void TraceFileSelectionTabCallback();
	void TraceFileSelectionWindowCallback();

	bool DrawDetailedTraceSelect();
	// called before every single_trace_info_tab_fn
	void SingleTraceInfoTabHeader(tr_tick activeTick);

	// a tab that shows detailed info for a single trace at a single tick, assumes TrReadContentScope is already set
	using single_trace_info_tab_fn = void (*)(tr_tick activeTick);
	// these are of type single_trace_info_tab_fn
	void PlayerTabCallback(tr_tick activeTick);
	void EntityTabCallback(tr_tick activeTick);
	void PortalTabCallback(tr_tick activeTick);

	void RenderStyleTab();
} // namespace player_trace::tr_imgui

#endif