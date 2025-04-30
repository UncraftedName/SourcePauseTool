#include <stdafx.hpp>

#include "tr_plot_cache.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

using namespace player_trace;

void TrPlotCache::SyncWithTrace()
{
	if (tr->numRecordedTicks <= numSyncedTicks)
		return;

	TrReadContextScope scope{*tr};

	auto serverStateIdx = tr->GetAtTick<TrServerState>(numSyncedTicks);
	auto playerDataIdx = tr->GetAtTick<TrPlayerData>(numSyncedTicks);
	if (!playerDataIdx.IsValid())
		return;

	for (uint32_t tick = numSyncedTicks; tick < tr->numRecordedTicks; numSyncedTicks = ++tick)
	{
		auto updateIdx = []<typename T>(uint32_t toTick, TrIdx<T>& idx)
		{
			if ((idx + 1).IsValid() && (idx + 1)->tick >= toTick)
				++idx;
		};

		updateIdx(tick, playerDataIdx);
		updateIdx(tick, serverStateIdx);

		auto& pd = **playerDataIdx;

		PlotPoint newPt{
		    .tick = (float)tick,
		    .unpausedTicksSinceStart = (float)datas[TR_PDT_WITHOUT_PAUSES].plotPoints.size(),
		    .qPhysPos = pd.qPosIdx.IsValid() ? **pd.qPosIdx : Vector{NAN},
		    .qPhysVel = pd.qVelIdx.IsValid() ? **pd.qVelIdx : Vector{NAN},
		    .vPhysPos = pd.transVPhysIdx.IsValid() && pd.transVPhysIdx->posIdx->IsValid()
		                    ? **pd.transVPhysIdx->posIdx
		                    : Vector{NAN},
		    .vPhysVel = pd.vVelIdx.IsValid() ? **pd.vVelIdx : Vector{NAN},
		};

		for (int i = 0; i < TR_PDT_COUNT; i++)
		{
			if (i == TR_PDT_WITHOUT_PAUSES && (!serverStateIdx.IsValid() || serverStateIdx->paused))
				break;
			PlotData& data = datas[i];
			data.plotPoints.push_back(newPt);
			data.anyQPhysPos |= newPt.qPhysPos.IsValid();
			data.anyQPhysVel |= newPt.qPhysVel.IsValid();
			data.anyVPhysPos |= newPt.vPhysPos.IsValid();
			data.anyVPhysVel |= newPt.vPhysVel.IsValid();
		}
	}
}

#endif
