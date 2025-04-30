#include "stdafx.hpp"

#include "tr_imgui_cache.hpp"

#include "thirdparty/implot/implot.h"

#ifdef SPT_PLAYER_TRACE_ENABLED

using namespace player_trace;

void TrImGuiCache::UpdateDataBuffers()
{
	if (tr->numRecordedTicks <= numTicksInBuffers)
		return;

	auto serverStateIdx = tr->GetAtTick<TrServerState>(numTicksInBuffers);
	auto playerDataIdx = tr->GetAtTick<TrPlayerData>(numTicksInBuffers);
	if (!playerDataIdx.IsValid())
		return;

	for (uint32_t tick = numTicksInBuffers; tick < tr->numRecordedTicks; numTicksInBuffers = ++tick)
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
		    .unpausedTicksSinceStart = (float)datas[DATA_WITHOUT_PAUSES].plotPoints.size(),
		    .qPhysPos = pd.qPosIdx.IsValid() ? **pd.qPosIdx : Vector{NAN},
		    .qPhysVel = pd.qVelIdx.IsValid() ? **pd.qVelIdx : Vector{NAN},
		    .vPhysPos = pd.transVPhysIdx.IsValid() && pd.transVPhysIdx->posIdx->IsValid()
		                    ? **pd.transVPhysIdx->posIdx
		                    : Vector{NAN},
		    .vPhysVel = pd.vVelIdx.IsValid() ? **pd.vVelIdx : Vector{NAN},
		};

		for (int i = 0; i < DATA_COUNT; i++)
		{
			if (i == DATA_WITHOUT_PAUSES && (!serverStateIdx.IsValid() || serverStateIdx->paused))
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

void TrImGuiCache::DrawImGuiPlots(tr_tick activeTick)
{
	PlotData& data = datas[config.onlyUnpausedTicks ? DATA_WITHOUT_PAUSES : DATA_WITH_PAUSES];
	int nPlotPoints = (int)data.plotPoints.size();

	ImPlotSubplotFlags subPlotFlags = ImPlotSubplotFlags_None;
	ImPlotFlags plotFlags = ImPlotFlags_Crosshairs;
	if (config.linkX)
		subPlotFlags |= ImPlotSubplotFlags_LinkAllX;

	bool subPlotRet = ImPlot::BeginSubplots("##player_trace",
	                                        config.posAndVelSelect == PVS_POS_AND_VEL ? 2 : 1,
	                                        1,
	                                        ImVec2{-1, -1},
	                                        subPlotFlags);

	if (subPlotRet)
	{
		ImPlotAxisFlags axisFlags = ImPlotAxisFlags_None;
		if (config.autoFit)
			axisFlags |= ImPlotAxisFlags_AutoFit;

		int off;
		int count;

		if (config.autoFit && config.limitAutoFit)
		{
			off = config.maxElems > nPlotPoints ? 0 : nPlotPoints - config.maxElems;
			count = MIN(nPlotPoints, config.maxElems);
		}
		else
		{
			off = 0;
			count = nPlotPoints;
		}

		auto dataPtrFn = [=](size_t structOff)
		{ return nPlotPoints == 0 ? nullptr : (float*)((char*)&data.plotPoints[off] + structOff); };

		float* xs = dataPtrFn(config.onlyUnpausedTicks ? offsetof(PlotPoint, unpausedTicksSinceStart)
		                                               : offsetof(PlotPoint, tick));
		const char* xAxName = config.onlyUnpausedTicks ? "server ticks since start" : "tick";

		ImPlotLineFlags lineFlags = ImPlotLineFlags_SkipNaN;

		auto plotXyzLines = [=](const char* legendFmt, size_t startStructOffset, bool forceHide, bool hide)
		{
			if (forceHide)
				ImPlot::HideNextItem(hide, ImPlotCond_Always);
			for (int i = 0; i < 3; i++)
			{
				char buf[32];
				snprintf(buf, sizeof buf, legendFmt, 'X' + i);
				ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, config.lineThickness);
				ImPlot::PlotLine(buf,
				                 xs,
				                 dataPtrFn(startStructOffset + sizeof(float) * i),
				                 count,
				                 lineFlags,
				                 0,
				                 sizeof(PlotPoint));
			}
		};

		if (config.onlyUnpausedTicks)
		{
			auto svStateIdx = tr->GetAtTick<TrServerState>(activeTick);
			if (svStateIdx)
				activeTick = svStateIdx->GetServerTickFromThisAsLastState(activeTick);
		}

		ImVec4 activeTickCol{1.f, 1.f, 0.f, 1.f};

		auto plotTags = [=]() { ImPlot::TagX(activeTick, activeTickCol, "active"); };

		auto plotTagLines = [=]() { ImPlot::PlotInfLines("active", &activeTick, 1); };

		if ((config.posAndVelSelect & PVS_POS) && ImPlot::BeginPlot("", ImVec2{-1, -1}, plotFlags))
		{
			ImPlotAxisFlags xAxFlags = axisFlags;
			if (config.posAndVelSelect & PVS_VEL)
			{
				xAxFlags |= ImPlotAxisFlags_NoLabel;
				if (config.autoFit || config.linkX)
					xAxFlags |= ImPlotAxisFlags_NoTickLabels;
			}

			ImPlot::SetupAxes(xAxName, "pos", xAxFlags, axisFlags);
			ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, DBL_MAX);
			plotXyzLines("QPhys %c pos", offsetof(PlotPoint, qPhysPos), false, false);
			plotXyzLines("VPhys %c pos", offsetof(PlotPoint, vPhysPos), false, false);
			plotTagLines();
			if (!(xAxFlags & ImPlotAxisFlags_NoTickLabels))
				plotTags();
			ImPlot::EndPlot();
		}

		if ((config.posAndVelSelect & PVS_VEL) && ImPlot::BeginPlot("", ImVec2{-1, -1}, plotFlags))
		{
			ImPlot::SetupAxes(xAxName, "vel", axisFlags, axisFlags);
			ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, DBL_MAX);
			plotXyzLines("QPhys %c vel", offsetof(PlotPoint, qPhysVel), false, false);
			if (config.showXyVel)
			{
				ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, config.lineThickness);
				// TODO won't work if we're using non-paused ticks
				ImPlot::PlotLineG(
				    "QPhys XY vel",
				    [](int idx, void* arr)
				    { return ImPlotPoint{(double)idx, ((PlotPoint*)arr)[idx].qPhysVel.Length2D()}; },
				    data.plotPoints.data(),
				    nPlotPoints,
				    lineFlags);
			}
			plotXyzLines("VPhys %c vel", offsetof(PlotPoint, vPhysVel), false, false);
			plotTagLines();
			plotTags();
			ImPlot::EndPlot();
		}

		ImPlot::EndSubplots();
	}
}

void TrImGuiCache::ImGuiTabCallback(tr_tick activeTick)
{
	TrReadContextScope scope{*tr};
	UpdateDataBuffers();
	config.DrawImGuiConfig();
	if (!config.plotsInSeparateWindows)
		DrawImGuiPlots(activeTick);
}

bool TrImGuiCache::ImGuiWindowCallback(tr_tick activeTick)
{
	if (!config.plotsInSeparateWindows)
		return false;
	TrReadContextScope scope{*tr};
	UpdateDataBuffers();
	if (ImGui::Begin("Player trace plots", &config.plotsInSeparateWindows))
		DrawImGuiPlots(activeTick);
	ImGui::End();
	return config.plotsInSeparateWindows;
}

void TrImGuiCache::PersistPlotConfig::DrawImGuiConfig()
{
	ImGui::Checkbox("Only unpaused ticks", &onlyUnpausedTicks);
	ImGui::Checkbox("Auto-fit", &autoFit);
	ImGui::SliderFloat("Line thickness", &lineThickness, 1.f, 6.f, nullptr, ImGuiSliderFlags_AlwaysClamp);

	ImGui::BeginDisabled(!autoFit);
	ImGui::Checkbox("Limit auto-fit", &limitAutoFit);
	ImGui::SameLine();
	ImGui::BeginDisabled(!limitAutoFit);
	ImGui::DragInt("##maxElems",
	               &maxElems,
	               1.f,
	               2,
	               10000,
	               "show %d most recent ticks",
	               ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	ImGui::RadioButton("position only", (int*)&posAndVelSelect, PVS_POS);
	ImGui::SameLine();
	ImGui::RadioButton("velocity only", (int*)&posAndVelSelect, PVS_VEL);
	ImGui::SameLine();
	ImGui::RadioButton("position & velocity", (int*)&posAndVelSelect, PVS_POS_AND_VEL);

	ImGui::BeginDisabled(!(posAndVelSelect & PVS_VEL));
	ImGui::Checkbox("Show XY vel", &showXyVel);
	ImGui::EndDisabled();

	ImGui::RadioButton("QPhys only", (int*)&physTypeSelect, PTS_QPHYS);
	ImGui::SameLine();
	ImGui::RadioButton("VPhys only", (int*)&physTypeSelect, PTS_VPHYS);
	ImGui::SameLine();
	ImGui::RadioButton("QPhys & VPhys", (int*)&physTypeSelect, PTS_QPHYS_AND_VPHYS);

	ImGui::BeginDisabled(posAndVelSelect != PVS_POS_AND_VEL || autoFit);
	ImGui::Checkbox("link ticks between position and velocity plots", &linkX);
	ImGui::EndDisabled();

	ImGui::Checkbox("Display plot in separate window", &plotsInSeparateWindows);
}

#endif
