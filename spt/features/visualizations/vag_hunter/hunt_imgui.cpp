#include "stdafx.hpp"

#include "hunt.hpp"
#include "spt/utils/ent_utils.hpp"

#include <numeric>

inline ImVec4 BLUE_PORTAL_COLOR = ImGui::ColorConvertU32ToFloat4(Color32ToImU32({64, 160, 255, 127}));
inline ImVec4 ORANGE_PORTAL_COLOR = ImGui::ColorConvertU32ToFloat4(Color32ToImU32({255, 160, 32, 127}));

bool VagHunterHuntFeature::ImGuiPointTargetConfig(HtVagPointTarget& target)
{
	bool changed = false;

	if (target.points.empty())
	{
		ImGui::Text("No point targets");
	}
	else
	{
		ImGui::Text("%u point target(s)", target.points.size());
		ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;
		int removePointIdx = -1;
		ImVec2 tableContainerSize(0.f, ImGui::GetItemRectSize().y * 8);
		if (ImGui::BeginTable("point_targets", 1, flags, tableContainerSize))
		{
			ImGuiListClipper clipper;
			clipper.Begin(target.points.size());
			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
				{
					ImGui::PushID(row);
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					if (ImGui::SmallButton(ICON_CI_TRASH))
						removePointIdx = row;
					ImGui::SetItemTooltip("remove point target");
					ImGui::SameLine();
					const Vector& pt = target.points[row];
					ImGui::Text("%.9g %.9g %.9g", pt.x, pt.y, pt.z);
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}

		if (removePointIdx >= 0)
		{
			std::unique_lock lk{targetMtx};
			target.points.erase(target.points.begin() + removePointIdx);
			changed = true;
		}
	}

	if (ImGui::SmallButton(ICON_CI_PLUS ICON_CI_EYE))
	{
		std::unique_lock lk{targetMtx};
		target.points.push_back(utils::GetPlayerEyePosition());
		changed = true;
	}
	ImGui::SetItemTooltip("Add a point target at the player's eye location");

	return changed;
}

bool VagHunterHuntFeature::ImGuiBoxTargetConfig(HtVagBoxTarget& target)
{
	bool changed = false;

	if (target.aabbs.empty())
	{
		ImGui::Text("No box targets");
	}
	else
	{
		ImGui::Text("%u box target(s)", target.aabbs.size());
		ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;
		int removePointIdx = -1;
		ImVec2 tableContainerSize(0.f, ImGui::GetItemRectSize().y * 8);
		if (ImGui::BeginTable("box_targets", 1, flags, tableContainerSize))
		{
			ImGuiListClipper clipper;
			clipper.Begin(target.aabbs.size());
			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
				{
					ImGui::PushID(row);
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					if (ImGui::SmallButton(ICON_CI_TRASH))
						removePointIdx = row;
					ImGui::SetItemTooltip("remove AABB target");
					ImGui::SameLine();
					const HtAabb& aabb = target.aabbs[row];
					ImGui::Text("<%.9g %.9g %.9g> - <%.9g %.9g %.9g>",
					            aabb.mins.x,
					            aabb.mins.y,
					            aabb.mins.z,
					            aabb.maxs.x,
					            aabb.maxs.y,
					            aabb.maxs.z);
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}

		if (removePointIdx >= 0)
		{
			std::unique_lock lk{targetMtx};
			target.aabbs.erase(target.aabbs.begin() + removePointIdx);
			changed = true;
		}
	}

	Vector newPt = utils::GetPlayerEyePosition();

	ImGui::BeginDisabled(target.pendingAabbTargetCorner == newPt);
	if (ImGui::SmallButton(ICON_CI_PLUS))
	{
		if (target.pendingAabbTargetCorner.has_value())
		{
			Vector mins, maxs;
			VectorMin(target.pendingAabbTargetCorner.value(), newPt, mins);
			VectorMax(target.pendingAabbTargetCorner.value(), newPt, maxs);
			std::unique_lock lk{targetMtx};
			target.aabbs.emplace_back(mins, maxs);
			target.pendingAabbTargetCorner.reset();
			changed = true;
		}
		else
		{
			target.pendingAabbTargetCorner = newPt;
		}
	}

	if (target.pendingAabbTargetCorner.has_value())
		ImGui::SetItemTooltip("Set second AABB corner at player eye position");
	else
		ImGui::SetItemTooltip("Set first AABB corner at player eye position");

	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::BeginDisabled(!target.pendingAabbTargetCorner.has_value());
	if (ImGui::SmallButton("Cancel"))
		target.pendingAabbTargetCorner.reset();
	ImGui::EndDisabled();

	return changed;
}

void VagHunterHuntFeature::InitNewWorker()
{
	worker = std::make_unique<HtWorker>(generationSize, sampleRatios, std::make_shared<HtContinuousWorld>());
}

void VagHunterHuntFeature::ImGuiTabCallbackImpl()
{
	ImGui::SeparatorText("VAG target set");
	ImGui::PushID("point");
	if (ImGuiPointTargetConfig(vagTarget.pointTarget))
		vagTarget.RecalcMaxInternalDist();
	ImGui::PopID();
	ImGui::PushID("box");
	if (ImGuiBoxTargetConfig(vagTarget))
		vagTarget.RecalcMaxInternalDist();
	ImGui::PopID();

	// ImGui::Text("Debug: player eye distance to target: %.9g", vagTarget.DistTo(utils::GetPlayerEyePosition()));

	if (ImGui::Button("Do generation"))
	{
		if (!worker)
			InitNewWorker();
		worker->MakeNewGeneration();
	}

	ImGui::BeginDisabled(!worker);
	if (ImGui::Button("Stop"))
		worker.reset();
	if (worker)
		worker->DrawImGuiHistoryTable();
	ImGui::EndDisabled();
}

void HtWorker::DrawImGuiHistoryTable()
{
	std::unique_lock lk(mtx);

	if (sortedHistory.size() != candidateHistory.size())
	{
		for (candidate_idx i = sortedHistory.size(); i < candidateHistory.size(); i++)
			sortedHistory.push_back(i);

		std::ranges::sort(sortedHistory,
		                  std::less{},
		                  [this](candidate_idx i) { return candidateHistory[i].metric; });
	}

	ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
	ImVec2 outer_size = ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 8);
	if (ImGui::BeginTable("history_table", 3, tableFlags, outer_size))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("index");
		ImGui::TableSetupColumn("distance");
		ImGui::TableSetupColumn("entry");
		ImGui::TableHeadersRow();

		ImGuiListClipper clipper;
		clipper.Begin(sortedHistory.size());
		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
			{
				ImGui::TableNextRow();
				candidate_idx cIdx = sortedHistory[row];
				const auto& c = candidateHistory[cIdx];

				ImGui::TableSetColumnIndex(0);
				char buf[10];
				sprintf_s(buf, "%d", row);

				ImGuiSelectableFlags selectFlags = ImGuiSelectableFlags_SpanAllColumns;
				if (ImGui::Selectable(buf, cIdx == drawPortalsForIdx, selectFlags))
					drawPortalsForIdx = cIdx;
				if (ImGui::IsItemHovered())
					drawPortalsForIdx = cIdx;

				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%.7g", c.metric);

				ImGui::TableSetColumnIndex(2);
				if (c.entryColor == HT_ENTRY_BLUE)
					ImGui::TextColored(BLUE_PORTAL_COLOR, "blue");
				else
					ImGui::TextColored(ORANGE_PORTAL_COLOR, "orange");
			}
		}
		ImGui::EndTable();
	}
}
