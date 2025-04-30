#pragma once

#include "tr_structs.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

namespace player_trace
{
	enum TrPlotDataType
	{
		TR_PDT_WITH_PAUSES,
		TR_PDT_WITHOUT_PAUSES,
		TR_PDT_COUNT,
	};

	class TrPlotCache
	{
	private:
		friend struct TrPlayerTrace;
		const TrPlayerTrace* tr;

	public:
		struct PlotPoint
		{
			float tick;
			float unpausedTicksSinceStart;
			Vector qPhysPos, qPhysVel, vPhysPos, vPhysVel;
			float qPhysVelXy, vPhysVelXy;
		};

		struct PlotData
		{
			std::vector<PlotPoint> plotPoints;
			bool anyQPhysPos, anyVPhysPos, anyQPhysVel, anyVPhysVel;
		};

	private:
		tr_tick numSyncedTicks = 0;
		std::array<PlotData, TR_PDT_COUNT> datas{};

		void SyncWithTrace();

	public:
		TrPlotCache(const TrPlayerTrace& tr) : tr{&tr} {};
		TrPlotCache(const TrPlotCache&) = delete;

		const PlotData& GetData(TrPlotDataType which)
		{
			SyncWithTrace();
			return datas[which];
		}
	};
} // namespace player_trace

#endif
