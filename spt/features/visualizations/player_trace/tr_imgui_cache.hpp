#pragma once

#include "tr_structs.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

namespace player_trace
{
	class TrImGuiCache
	{
		friend struct TrPlayerTrace;

		const TrPlayerTrace* tr;

		struct PlotPoint
		{
			float tick;
			Vector qPhysPos, qPhysVel, vPhysPos, vPhysVel;
		};

		std::vector<PlotPoint> plotPoints;

		uint32_t updatedTillTick = 0;

		void Update(uint32_t toTick);

	public:
		TrImGuiCache(TrPlayerTrace& tr) : tr{&tr} {};
		TrImGuiCache(const TrImGuiCache&) = delete;

		void PlotTrace();
	};
} // namespace player_trace

#endif
