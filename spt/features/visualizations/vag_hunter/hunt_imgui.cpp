#include "stdafx.hpp"

#include "hunt.hpp"
#include "spt/utils/ent_utils.hpp"

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
		{
			worker = std::make_unique<HtWorker>(100,
			                                    HtGenerationInfoRatios::CreateReasonableRatios(),
			                                    std::make_shared<HtContinuousWorld>());
		}

		worker->MakeNewGeneration();
	}

	ImGui::BeginDisabled(!worker);
	if (ImGui::Button("Stop"))
		worker.reset();
	ImGui::EndDisabled();
}
