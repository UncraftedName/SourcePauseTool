#include "stdafx.hpp"

#include "tr_imgui.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "spt/features/visualizations/imgui/imgui_interface.hpp"

using namespace player_trace;

void tr_imgui::PortalTabCallback(ImGuiDetailedInfoTraceSelection& info)
{
	if (!tr_imgui::DrawDetailedTraceSelect(info))
		return;
	tr_imgui::DrawDetailedInfoHeader(info);

	auto& tr = info.tp->detailedImGuiTraceIt->second.tr;
	TrReadContextScope scope{tr};
	tr_tick activeTick = info.activeTick;

	auto portalSnap = tr.GetAtTick<TrPortalSnapshot>(activeTick);
	auto portalSp = *portalSnap.GetOrDefault().portalsSp;

	if (portalSp.empty())
	{
		ImGui::TextUnformatted("No portals");
		return;
	}

	// a simplified copy of the portal selection widget
	ImGuiTabBarFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoHostExtendX
	                              | ImGuiTableFlags_NoKeepColumnsVisible;
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

		for (auto portalIdx : portalSp)
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

#endif
