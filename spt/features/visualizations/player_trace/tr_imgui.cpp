#include "stdafx.hpp"

#include <inttypes.h>
#include <bitset>

#include "tr_imgui.hpp"
#include "tr_plot_cache.hpp"
#include "tr_entity_cache.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "thirdparty/implot/implot.h"

#include "spt/utils/game_detection.hpp"
#include "spt/features/visualizations/imgui/imgui_interface.hpp"

#define _VEC_FMT "<%f, %f, %f>"
#define _VEC_UNP(v) (v).x, (v).y, (v).z

using namespace player_trace;

enum TrPosAndVelSelect : int
{
	TR_PVS_POS = 1,
	TR_PVS_VEL = 1 << 1,
	TR_PVS_POS_AND_VEL = TR_PVS_POS | TR_PVS_VEL,
};

enum TrPhysTypeSelect : int
{
	TR_PTS_QPHYS = 1,
	TR_PTS_VPHYS = 1 << 1,
	TR_PTS_QPHYS_AND_VPHYS = TR_PTS_QPHYS | TR_PTS_VPHYS,
};

struct PlotConfig
{
	bool setupDone = false;
	bool plotsInSeparateWindows = false;
	bool autoFit = true;
	bool limitAutoFit = false;
	bool linkX = true;
	bool showXyVel = false;
	int maxElems = 1000;
	float lineThickness = -1.f;
	ImPlotColormap colorMap = -1;
	TrPlotDataType dataType = TR_PDT_WITH_PAUSES;
	TrPosAndVelSelect posAndVelSelect = TR_PVS_POS_AND_VEL;
	TrPhysTypeSelect physTypeSelect = TR_PTS_QPHYS_AND_VPHYS;

	void Setup();
	void DrawInImGui();

} inline static g_plotConfig;

static void TrDrawDataLine(const TrPlotCache::PlotData& data,
                           size_t structOffset,
                           ImPlotLineFlags lineFlags,
                           const char* legendFmt,
                           ...)
{
	char buf[32];
	va_list va;
	va_start(va, legendFmt);
	vsnprintf(buf, sizeof buf, legendFmt, va);
	va_end(va);

	ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, g_plotConfig.lineThickness);
	int nPlotPoints = (int)data.plotPoints.size();
	if (nPlotPoints > 0)
	{
		ImPlot::PlotLine(buf,
		                 (float*)&data.plotPoints[0].tick,
		                 (float*)((char*)&data.plotPoints[0] + structOffset),
		                 nPlotPoints,
		                 lineFlags,
		                 0,
		                 sizeof(TrPlotCache::PlotPoint));
	}
	else
	{
		ImPlot::PlotDummy(buf);
	}
}

static void TrDrawXyzPlots(const TrPlotCache::PlotData& data,
                           size_t startStructOffset,
                           ImPlotLineFlags lineFlags,
                           const char* legendFmt)
{
	for (int i = 0; i < 3; i++)
		TrDrawDataLine(data, startStructOffset + sizeof(float) * i, lineFlags, legendFmt, 'X' + i);
}

static void TrDrawTags(const TrPlotCache::PlotData& data, tr_tick activeTick, bool onlyLines)
{
	(void)data;

	ImPlot::PlotInfLines("tags", &activeTick, 1);
	if (onlyLines)
		return;

	ImVec4 activeTickCol{1.f, 1.f, 0.f, 1.f};
	ImPlot::TagX(activeTick, activeTickCol, "active");
}

static void TrDrawImGuiPlots(const TrPlayerTrace& tr, tr_tick activeTick)
{
	TrReadContextScope scope{tr};

	auto& cfg = g_plotConfig;
	const TrPlotCache::PlotData& data = tr.GetPlotCache().GetData(cfg.dataType);
	int nPlotPoints = (int)data.plotPoints.size();

	ImPlotSubplotFlags subPlotFlags = ImPlotSubplotFlags_None;
	ImPlotFlags plotFlags = ImPlotFlags_Crosshairs;
	if (cfg.linkX)
		subPlotFlags |= ImPlotSubplotFlags_LinkAllX;

	bool subPlotRet = ImPlot::BeginSubplots("##player_trace",
	                                        cfg.posAndVelSelect == TR_PVS_POS_AND_VEL ? 2 : 1,
	                                        1,
	                                        ImVec2{-1, -1},
	                                        subPlotFlags);

	if (subPlotRet)
	{
		ImPlotAxisFlags axisFlags = ImPlotAxisFlags_None;
		if (cfg.autoFit)
			axisFlags |= ImPlotAxisFlags_AutoFit;

		int off;
		int count;

		if (cfg.autoFit && cfg.limitAutoFit)
		{
			off = cfg.maxElems > nPlotPoints ? 0 : nPlotPoints - cfg.maxElems;
			count = MIN(nPlotPoints, cfg.maxElems);
		}
		else
		{
			off = 0;
			count = nPlotPoints;
		}

		const char* xAxName = cfg.dataType == TR_PDT_WITH_PAUSES ? "trace tick" : "server ticks since start";

		ImPlotLineFlags lineFlags = ImPlotLineFlags_SkipNaN;

		if (cfg.dataType == TR_PDT_WITHOUT_PAUSES)
		{
			auto svStateIdx = tr.GetAtTick<TrServerState>(activeTick);
			if (svStateIdx.IsValid())
				activeTick = svStateIdx->GetServerTickFromThisAsLastState(activeTick);
		}

		if ((cfg.posAndVelSelect & TR_PVS_POS) && ImPlot::BeginPlot("", ImVec2{-1, -1}, plotFlags))
		{
			ImPlotAxisFlags xAxFlags = axisFlags;
			if (cfg.posAndVelSelect & TR_PVS_VEL)
			{
				xAxFlags |= ImPlotAxisFlags_NoLabel;
				if (cfg.autoFit || cfg.linkX)
					xAxFlags |= ImPlotAxisFlags_NoTickLabels;
			}

			ImPlot::SetupAxes(xAxName, "pos", xAxFlags, axisFlags);
			ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, DBL_MAX);
			ImPlot::PushColormap(cfg.colorMap);
			TrDrawXyzPlots(data, offsetof(TrPlotCache::PlotPoint, qPhysPos), lineFlags, "QPhys %c pos");
			TrDrawXyzPlots(data, offsetof(TrPlotCache::PlotPoint, vPhysPos), lineFlags, "VPhys %c pos");
			TrDrawTags(data, activeTick, xAxFlags & ImPlotAxisFlags_NoTickLabels);
			ImPlot::PopColormap();
			ImPlot::EndPlot();
		}

		if ((cfg.posAndVelSelect & TR_PVS_VEL) && ImPlot::BeginPlot("", ImVec2{-1, -1}, plotFlags))
		{
			ImPlot::SetupAxes(xAxName, "vel", axisFlags, axisFlags);
			ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, DBL_MAX);
			ImPlot::PushColormap(cfg.colorMap);
			TrDrawXyzPlots(data, offsetof(TrPlotCache::PlotPoint, qPhysVel), lineFlags, "QPhys %c vel");
			if (cfg.showXyVel)
			{
				AssertMsg(0, "setup colormap");
				ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, cfg.lineThickness);
				// TODO won't work if we're using non-paused ticks
				ImPlot::PlotLineG(
				    "QPhys XY vel",
				    [](int idx, void* arr)
				    {
					    return ImPlotPoint{(double)idx,
					                       ((TrPlotCache::PlotPoint*)arr)[idx].qPhysVel.Length2D()};
				    },
				    (void*)data.plotPoints.data(),
				    nPlotPoints,
				    lineFlags);
			}
			TrDrawXyzPlots(data, offsetof(TrPlotCache::PlotPoint, vPhysVel), lineFlags, "VPhys %c vel");
			TrDrawTags(data, activeTick, false);
			ImPlot::PopColormap();
			ImPlot::EndPlot();
		}

		ImPlot::EndSubplots();
	}
}

static void TrDrawPlayerData(const TrPlayerTrace& tr, tr_tick activeTick)
{
	auto plIdx = tr.GetAtTick<TrPlayerData>(activeTick);
	TrPlayerData defaultPl{};
	auto& pl = plIdx.IsValid() ? **plIdx : defaultPl;
	Vector vecInvalid{NAN, NAN, NAN};
	QAngle angInvalid{NAN, NAN, NAN};
	TrTransform transInvalid{};

	Vector eyePos, sgEyePos, vPhysPos;
	QAngle eyeAng, sgEyeAng, vPhysAng;
	pl.transVPhysIdx.GetOrDefault(transInvalid).GetPosAng(vPhysPos, vPhysAng);
	pl.transEyesIdx.GetOrDefault(transInvalid).GetPosAng(eyePos, eyeAng);
	pl.transSgEyesIdx.GetOrDefault(transInvalid).GetPosAng(sgEyePos, sgEyeAng);

	ImGui::Text("QPhys pos: " _VEC_FMT, _VEC_UNP(pl.qPosIdx.GetOrDefault(vecInvalid)));
	ImGui::Text("VPhys pos: " _VEC_FMT, _VEC_UNP(vPhysPos));
	ImGui::Text("VPhys ang: " _VEC_FMT, _VEC_UNP(vPhysAng));
	ImGui::Text("QPhys vel: " _VEC_FMT, _VEC_UNP(pl.qVelIdx.GetOrDefault(vecInvalid)));
	if (tr.GetFirstExportVersion<TrPlayerData>() >= 2)
		ImGui::Text("VPhys vel: " _VEC_FMT, _VEC_UNP(pl.vVelIdx.GetOrDefault(vecInvalid)));
	else
		ImGui::TextDisabled("VPhys vel: data was not recorded on this trace version");
	ImGui::Text("Eye pos: " _VEC_FMT, _VEC_UNP(eyePos));
	ImGui::Text("Eye ang: " _VEC_FMT, _VEC_UNP(eyeAng));
	if (utils::DoesGameLookLikePortal())
	{
		if (eyePos == sgEyePos)
		{
			ImGui::TextDisabled("SG eye pos: same as eye pos");
			ImGui::TextDisabled("SG eye ang: same as eye ang");
		}
		else
		{
			ImGui::Text("SG eye pos: " _VEC_FMT, _VEC_UNP(sgEyePos));
			ImGui::Text("SG eye ang: " _VEC_FMT, _VEC_UNP(sgEyeAng));
		}
	}
	ImGui::Text("m_fFlags: %d", pl.m_fFlags);
	ImGui::Text("fov: %" PRIu32, pl.fov);
	ImGui::Text("health: %" PRIu32, pl.m_iHealth);
	ImGui::Text("life state: %" PRIu32, pl.m_lifeState);
	ImGui::Text("collision group: %" PRIu32, pl.m_CollisionGroup);
	ImGui::Text("move type: %" PRIu32, pl.m_MoveType);

	auto contactSp = *pl.contactPtsSp;

	if (ImGui::TreeNodeEx("contact_pts",
	                      ImGuiTreeNodeFlags_None,
	                      "%" PRIu32 " contact point%s",
	                      contactSp.size(),
	                      contactSp.size() == 1 ? "" : "s"))
	{
		for (auto contactIdx : contactSp)
		{
			const TrPlayerContactPoint& pcp = contactIdx.GetOrDefault();
			if (SptImGui::BeginBordered())
			{
				ImGui::PushID((int)contactIdx);
				ImGui::Text("object: '%s' (player is object %d)", *pcp.objNameIdx, !pcp.playerIsObj0);
				ImGui::Text("pos: " _VEC_FMT, _VEC_UNP(pcp.posIdx.GetOrDefault(vecInvalid)));
				ImGui::Text("normal: " _VEC_FMT, _VEC_UNP(pcp.normIdx.GetOrDefault(vecInvalid)));
				ImGui::Text("force: %f", pcp.force);
				ImGui::PopID();
				SptImGui::EndBordered();
			}
		}
		ImGui::TreePop();
	}
}

static void TrDrawEntityInfo(const TrEntityCache::EntMap& entMap)
{
	static std::vector<std::pair<TrIdx<TrEnt>, TrIdx<TrEntTransform>>> sortedEnts;
	sortedEnts.resize(entMap.size());
	std::ranges::transform(entMap, sortedEnts.begin(), std::identity{});
	std::ranges::sort(sortedEnts, std::less{}, [](auto& p) { return p.first->handle.GetEntryIndex(); });

	std::bitset<MAX_EDICTS> filterPassedSet{};
	int nPassed = 0;

	static ImGuiTextFilter filter;
	filter.Draw();
	ImGui::SetItemTooltip(
	    "Filter usage:\n"
	    "  \"\"         display all lines\n"
	    "  \"xxx\"      display lines containing \"xxx\"\n"
	    "  \"xxx,yyy\"  display lines containing \"xxx\" or \"yyy\"\n"
	    "  \"-xxx\"     hide lines containing \"xxx\"");

	for (auto [entIdx, _] : sortedEnts)
	{
		const TrEnt& ent = **entIdx;
		auto objSp = *ent.physSp;

		bool passFilter = !filter.IsActive();
		if (!passFilter)
			passFilter = filter.PassFilter(*ent.classNameIdx);
		if (!passFilter)
			passFilter = filter.PassFilter(*ent.networkClassNameIdx);
		if (!passFilter)
			passFilter = filter.PassFilter(*ent.nameIdx);
		for (size_t i = 0; !passFilter && i < objSp.size(); i++)
			passFilter = filter.PassFilter(*objSp[i]->infoIdx->nameIdx);

		// special case for portals, I accidentally collected them in old versions
		if (!strcmp(*ent.classNameIdx, "prop_portal")) [[unlikely]]
			passFilter = false;

		filterPassedSet[ent.handle.GetEntryIndex()] = passFilter;
		if (passFilter)
			nPassed++;
	}

	if (filter.IsActive())
	{
		ImGui::SameLine();
		ImGui::Text("(showing %d results)", nPassed);
	}

	for (auto [entIdx, entTransIdx] : sortedEnts)
	{
		const TrEnt& ent = **entIdx;

		if (!filterPassedSet[ent.handle.GetEntryIndex()])
			continue;

		ImGui::PushID(ent.handle.GetEntryIndex());
		const char* entName = *ent.nameIdx;
		if (ImGui::TreeNodeEx("ent_entry",
		                      ImGuiTreeNodeFlags_None,
		                      "index %d [%s]%s%s%s",
		                      ent.handle.GetEntryIndex(),
		                      *ent.networkClassNameIdx,
		                      *entName ? " \"" : "",
		                      entName,
		                      *entName ? "\"" : ""))
		{
			if (SptImGui::BeginBordered())
			{
				const TrEntTransform& entTrans = **entTransIdx;
				{
					Vector mins{NAN}, maxs{NAN}, pos{NAN};
					QAngle ang{NAN, NAN, NAN};
					if (entTrans.obbIdx.IsValid())
						entTrans.obbIdx->GetMinsMaxs(mins, maxs);
					if (entTrans.obbTransIdx.IsValid())
						entTrans.obbTransIdx->GetPosAng(pos, ang);
					ImGui::Text("OBB mins: " _VEC_FMT, _VEC_UNP(mins));
					ImGui::Text("OBB maxs: " _VEC_FMT, _VEC_UNP(maxs));
					ImGui::Text("server pos: " _VEC_FMT, _VEC_UNP(pos));
					ImGui::Text("server ang: " _VEC_FMT, _VEC_UNP(ang));
				}
				ImGui::Text("serial: %d", ent.handle.GetSerialNumber());
				ImGui::Text("class name: \"%s\"", *ent.classNameIdx);
				ImGui::Text("solid type: %d", ent.m_nSolidType);
				ImGui::Text("solid flags: %d", ent.m_usSolidFlags);
				ImGui::Text("collision group: %d", ent.m_CollisionGroup);

				auto objSp = *ent.physSp;
				if (objSp.empty())
				{
					ImGui::Text("0 physics objects");
				}
				else if (ImGui::TreeNodeEx("phys_objs",
				                           ImGuiTreeNodeFlags_None,
				                           "%" PRIu32 " physics object%s",
				                           objSp.size(),
				                           objSp.size() == 1 ? "" : "s"))
				{
					auto objTransSp = *entTrans.physTransSp;
					Assert(objSp.size() == objTransSp.size());
					for (uint32_t i = 0; i < objSp.size(); i++)
					{
						const TrPhysicsObject& obj = **objSp[i];
						const TrPhysicsObjectInfo& objInfo = **obj.infoIdx;
						const TrPhysMesh_v1& objMesh = **obj.meshIdx;
						const TrTransform& objTrans = **objTransSp[i];

						ImGui::PushID((int)objSp[i]);
						if (SptImGui::BeginBordered())
						{
							ImGui::Text("name: \"%s\"", *objInfo.nameIdx);
							ImGui::Text(
							    "asleep: %d, moveable: %d, trigger: %d, gravity enabled: %d",
							    !!(objInfo.flags & TR_POF_ASLEEP),
							    !!(objInfo.flags & TR_POF_MEOVEABLE),
							    !!(objInfo.flags & TR_POF_IS_TRIGGER),
							    !!(objInfo.flags & TR_POF_GRAVITY_ENABLED));

							Vector pos{NAN};
							QAngle ang{NAN, NAN, NAN};
							objTrans.GetPosAng(pos, ang);
							ImGui::Text("pos: " _VEC_FMT, _VEC_UNP(pos));
							ImGui::Text("ang: " _VEC_FMT, _VEC_UNP(ang));

							if (objMesh.ballRadius > 0)
							{
								ImGui::Text("ball mesh of radius %f",
								            objMesh.ballRadius);
							}
							else
							{
								ImGui::Text("mesh with %" PRIu32 " verts",
								            objMesh.vertIdxSp.n);
							}
							SptImGui::EndBordered();
						}
						ImGui::PopID();
					}
					ImGui::TreePop();
				}
				SptImGui::EndBordered();
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
}

static void TrDrawPortalInfo(std::span<const TrIdx<TrPortal>> portalIdxSp)
{
	if (portalIdxSp.empty())
		return;

	// a simplified copy of the portal selection widget
	ImGuiTabBarFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX;
	if (ImGui::BeginTable("##portal_select", 11, tableFlags))
	{
		ImGui::TableSetupColumn("index", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("linked", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("color", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("x", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("y", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("z", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("pitch", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("yaw", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("roll", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("linkage", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableHeadersRow();

		for (auto portalIdx : portalIdxSp)
		{
			auto& portal = portalIdx.GetOrDefault();
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%d", portal.handle.GetEntryIndex());
			ImGui::TableSetColumnIndex(1);
			if (portal.linkedHandle.IsValid())
				ImGui::Text("%d", portal.linkedHandle.GetEntryIndex());
			else
				ImGui::TextDisabled("NONE");
			ImGui::TableSetColumnIndex(2);
			ImGui::TextColored(portal.isOrange ? ImVec4{1.f, .63f, .13f, 1.f}
			                                   : ImVec4{.25f, .63f, 1.f, 1.f},
			                   portal.isOrange ? "orange" : "blue");
			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted(portal.isOpen ? "open" : (portal.isActivated ? "closed" : "inactive"));
			TrTransform trans = portal.transIdx.GetOrDefault();
			Vector pos;
			QAngle ang;
			trans.GetPosAng(pos, ang);
			for (int i = 0; i < 3; i++)
			{
				ImGui::TableSetColumnIndex(4 + i);
				ImGui::Text("%f", pos[i]);
				ImGui::TableSetColumnIndex(7 + i);
				ImGui::Text("%f", ang[i]);
			}
			ImGui::TableSetColumnIndex(10);
			ImGui::Text("%d", portal.linkageId);
		}

		ImGui::EndTable();
	}
}

void tr_imgui::WindowCallback(const TrPlayerTrace& tr, tr_tick activeTick)
{
	auto& cfg = g_plotConfig;
	if (!cfg.plotsInSeparateWindows)
		return;
	if (ImGui::Begin("Player trace plots", &cfg.plotsInSeparateWindows))
		TrDrawImGuiPlots(tr, activeTick);
	ImGui::End();
}

void PlotConfig::Setup()
{
	if (setupDone)
		return;

	// interpolate default lineThickness - 3.5 looks good on 4k but is too thick on smaller monitors
	lineThickness = 1.f + (3.5f - 1.f) / 3840.f * ImGui::GetMainViewport()->Size.x;

	static const ImU32 cmData[] = {
	    4279268351,
	    4280410108,
	    4278238984,
	    4292607744,
	    4294911764,
	    4294847198,
	    ImGui::ColorConvertFloat4ToU32({1, 1, 1, 1}), // dummy color for tags
	};
	colorMap = ImPlot::AddColormap("Player trace", cmData, ARRAYSIZE(cmData));

	setupDone = true;
}

void PlotConfig::DrawInImGui()
{
	Setup();
	bool onlyUnpaused = dataType == TR_PDT_WITHOUT_PAUSES;
	if (ImGui::Checkbox("Only unpaused ticks", &onlyUnpaused))
		dataType = onlyUnpaused ? TR_PDT_WITHOUT_PAUSES : TR_PDT_WITH_PAUSES;
	ImGui::Checkbox("Auto-fit", &autoFit);
	ImGui::SliderFloat("Line thickness", &lineThickness, 1.f, 6.f, "%.1f", ImGuiSliderFlags_AlwaysClamp);

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

	ImGui::RadioButton("position only", (int*)&posAndVelSelect, TR_PVS_POS);
	ImGui::SameLine();
	ImGui::RadioButton("velocity only", (int*)&posAndVelSelect, TR_PVS_VEL);
	ImGui::SameLine();
	ImGui::RadioButton("position & velocity", (int*)&posAndVelSelect, TR_PVS_POS_AND_VEL);

	ImGui::BeginDisabled(!(posAndVelSelect & TR_PVS_VEL));
	ImGui::Checkbox("Show XY vel", &showXyVel);
	ImGui::EndDisabled();

	ImGui::RadioButton("QPhys only", (int*)&physTypeSelect, TR_PTS_QPHYS);
	ImGui::SameLine();
	ImGui::RadioButton("VPhys only", (int*)&physTypeSelect, TR_PTS_VPHYS);
	ImGui::SameLine();
	ImGui::RadioButton("QPhys & VPhys", (int*)&physTypeSelect, TR_PTS_QPHYS_AND_VPHYS);

	ImGui::BeginDisabled(posAndVelSelect != TR_PVS_POS_AND_VEL || autoFit);
	ImGui::Checkbox("link ticks between position and velocity plots", &linkX);
	ImGui::EndDisabled();

	ImGui::Checkbox("Display plot in separate window", &plotsInSeparateWindows);
}

void tr_imgui::PlotTabCallback(const TrPlayerTrace& tr, tr_tick activeTick)
{
	g_plotConfig.DrawInImGui();
	if (!g_plotConfig.plotsInSeparateWindows)
	{
		ImGui::SetNextWindowSizeConstraints(ImVec2{0, 500.f}, ImVec2{FLT_MAX, FLT_MAX});
		ImGuiChildFlags flags = ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY;
		if (ImGui::BeginChild("graph_child", ImVec2{-1, -1}, flags))
			TrDrawImGuiPlots(tr, activeTick);
		ImGui::EndChild();
	}
}

void tr_imgui::EntityTabCallback(const TrPlayerTrace& tr, tr_tick activeTick)
{
	TrReadContextScope scope{tr};

	ImGui::Text("Active tick: %" PRIu32, activeTick);

	ImVec2 childSize{0, 0};
	ImGuiChildFlags childFlags = ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY;

	if (ImGui::CollapsingHeader("Player data"))
	{
		if (ImGui::BeginChild("player_data", childSize, childFlags))
			TrDrawPlayerData(tr, activeTick);
		ImGui::EndChild();
	}

	{
		auto& entMap = tr.GetEntityCache().GetEnts(activeTick);
		char buf[32];
		snprintf(buf, sizeof buf, "%" PRIu32 " entit%s###ent", entMap.size(), entMap.size() == 1 ? "y" : "ies");
		if (ImGui::CollapsingHeader(buf))
		{
			if (ImGui::BeginChild("ents", childSize, childFlags))
				TrDrawEntityInfo(entMap);
			ImGui::EndChild();
		}
	}

	{
		auto portalSnap = tr.GetAtTick<TrPortalSnapshot>(activeTick);
		auto portalSp = *portalSnap.GetOrDefault().portalsSp;
		char buf[32];
		snprintf(buf,
		         sizeof buf,
		         "%" PRIu32 " portal%s###portals",
		         portalSp.size(),
		         portalSp.size() == 1 ? "" : "s");
		if (ImGui::CollapsingHeader(buf) && !portalSp.empty()) // don't short circuit on .empty()
		{
			if (ImGui::BeginChild("portals", childSize, childFlags))
				TrDrawPortalInfo(portalSp);
			ImGui::EndChild();
		}
	}
}

void tr_imgui::TraceConfigTabCallback() {}

/*
* void PlayerTraceFeature::ImGuiTabCallback()
{
	imGuiWindowCallbackCalledNFramesWithoutTabCallback = 0;
	imGuiActive = true;
	spt_player_trace_feat.ClampActiveTick();
	spt_player_trace_feat.tr.GetImGuiCache().ImGuiTabCallback(spt_player_trace_feat.activeDrawTick);
}

void PlayerTraceFeature::ImGuiWindowCallback()
{
	// recreate the ImGui cache if we load a new trace
	if (!imGuiActive && !spt_player_trace_feat.tr.HasImGuiCache())
		return;
	spt_player_trace_feat.ClampActiveTick();
	imGuiActive = spt_player_trace_feat.tr.GetImGuiCache().ImGuiWindowCallback(spt_player_trace_feat.activeDrawTick)
	              || imGuiWindowCallbackCalledNFramesWithoutTabCallback < 3;
	if (imGuiActive)
		++imGuiWindowCallbackCalledNFramesWithoutTabCallback;
	else
		spt_player_trace_feat.tr.StopRenderingImGui();
}
*/

#endif
