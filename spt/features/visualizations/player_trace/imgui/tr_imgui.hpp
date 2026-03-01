#pragma once

#include "../tr_structs.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#define TR_IMG_VEC_FMT "<%f, %f, %f>"
#define TR_IMG_VEC_UNP(v) (v).x, (v).y, (v).z

namespace player_trace::tr_imgui
{
	void DrawActiveTraceInfo(tr_tick activeTick);

	void PlayerTabCallback(tr_tick activeTick);
	void EntityTabCallback(tr_tick activeTick);
	void PortalTabCallback(tr_tick activeTick);

} // namespace player_trace::tr_imgui

#endif