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
			float unpausedTicksSinceStart;
			Vector qPhysPos, qPhysVel, vPhysPos, vPhysVel;
			float qPhysVelXy, vPhysVelXy;
		};

		enum
		{
			DATA_WITH_PAUSES,
			DATA_WITHOUT_PAUSES,
			DATA_COUNT,
		};

		struct PlotData
		{
			std::vector<PlotPoint> plotPoints;
			bool anyQPhysPos, anyVPhysPos, anyQPhysVel, anyVPhysVel;
		} datas[DATA_COUNT]{};

		uint32_t numTicksInBuffers = 0;

		enum PosAndVelSelect : int
		{
			PVS_POS = 1,
			PVS_VEL = 1 << 1,
			PVS_POS_AND_VEL = PVS_POS | PVS_VEL,
		};

		enum PhysTypeSelect : int
		{
			PTS_QPHYS = 1,
			PTS_VPHYS = 1 << 1,
			PTS_QPHYS_AND_VPHYS = PTS_QPHYS | PTS_VPHYS,
		};

		struct PersistPlotConfig
		{
			bool plotsInSeparateWindows = false;
			bool autoFit = true;
			bool limitAutoFit = false;
			bool linkX = true;
			bool onlyUnpausedTicks = false;
			bool showXyVel = false;
			int maxElems = 1000;
			float lineThickness = 3.5f;

			PosAndVelSelect posAndVelSelect = PVS_POS_AND_VEL;
			PhysTypeSelect physTypeSelect = PTS_QPHYS_AND_VPHYS;

			void DrawImGuiConfig();

		} inline static config;

		void UpdateDataBuffers();

		void DrawImGuiPlots(tr_tick activeTick);

	public:
		TrImGuiCache(TrPlayerTrace& tr) : tr{&tr} {};
		TrImGuiCache(const TrImGuiCache&) = delete;

		void ImGuiTabCallback(tr_tick activeTick);
		bool ImGuiWindowCallback(tr_tick activeTick);
	};
} // namespace player_trace

#endif
