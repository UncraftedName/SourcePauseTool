#pragma once

#include "tr_structs.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

namespace player_trace::tr_imgui
{
	void PlotTabCallback(const TrPlayerTrace& tr, tr_tick activeTick);
	void EntityTabCallback(const TrPlayerTrace& tr, tr_tick activeTick);
	void TraceConfigTabCallback();
	void WindowCallback(const TrPlayerTrace& tr, tr_tick activeTick);

} // namespace player_trace::tr_imgui

#endif