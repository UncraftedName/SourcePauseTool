#include "stdafx.hpp"

#include "tr_imgui_cache.hpp"

#include "thirdparty/implot/implot.h"

#ifdef SPT_PLAYER_TRACE_ENABLED

using namespace player_trace;

void TrImGuiCache::Update(uint32_t toTick)
{
	if (toTick <= updatedTillTick)
		return;
	auto playerDataIdx = tr->GetAtTick<TrPlayerData_v1>(updatedTillTick);
	if (!playerDataIdx.IsValid())
		return;

	for (uint32_t tick = updatedTillTick; tick < toTick; updatedTillTick = tick++)
	{
		if (tick > updatedTillTick)
		{
			auto& playerData = **playerDataIdx;
			PlotPoint newPt{
			    .tick = (float)tick,
			    .qPhysPos = playerData.qPosIdx.IsValid() ? **playerData.qPosIdx : Vector{NAN},
			    .qPhysVel = playerData.qVelIdx.IsValid() ? **playerData.qVelIdx : Vector{NAN},
			    .vPhysPos =
			        playerData.transVPhysIdx.IsValid() && playerData.transVPhysIdx->posIdx->IsValid()
			            ? **playerData.transVPhysIdx->posIdx
			            : Vector{NAN},
			    .vPhysVel = playerData.vVelIdx.IsValid() ? **playerData.vVelIdx : Vector{NAN},
			};
			plotPoints.push_back(newPt);
		}
		if ((playerDataIdx + 1).IsValid() && (playerDataIdx + 1)->tick >= tick)
			++playerDataIdx;
	}
}

void TrImGuiCache::PlotTrace()
{
	TrReadContextScope scope{*tr};

	Update(tr->numRecordedTicks);
	if (ImPlot::BeginPlot("Player Trace"))
	{
		ImPlot::SetupAxes("tick", "pos");
		ImPlot::PlotLine("QPhys X Pos",
		                 (float*)((char*)plotPoints.data() + offsetof(PlotPoint, tick)),
		                 (float*)((char*)plotPoints.data() + offsetof(PlotPoint, qPhysPos.x)),
		                 updatedTillTick,
		                 0,
		                 0,
		                 sizeof(PlotPoint));
		ImPlot::PlotLine("QPhys Y Pos",
		                 (float*)((char*)plotPoints.data() + offsetof(PlotPoint, tick)),
		                 (float*)((char*)plotPoints.data() + offsetof(PlotPoint, qPhysPos.y)),
		                 updatedTillTick,
		                 0,
		                 0,
		                 sizeof(PlotPoint));
		ImPlot::PlotLine("QPhys Z Pos",
		                 (float*)((char*)plotPoints.data() + offsetof(PlotPoint, tick)),
		                 (float*)((char*)plotPoints.data() + offsetof(PlotPoint, qPhysPos.z)),
		                 updatedTillTick,
		                 0,
		                 0,
		                 sizeof(PlotPoint));
		ImPlot::EndPlot();
	}
}

#endif
