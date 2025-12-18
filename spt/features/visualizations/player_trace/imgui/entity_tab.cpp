#include "stdafx.hpp"

#include <bitset>

#include "tr_imgui.hpp"
#include "../tr_entity_cache.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "spt/utils/game_detection.hpp"
#include "spt/features/visualizations/imgui/imgui_interface.hpp"

using namespace player_trace;

void tr_imgui::EntityTabCallback(ImGuiDetailedInfoTraceSelection& info)
{
	if (!tr_imgui::DrawDetailedTraceSelect(info))
		return;
	tr_imgui::DrawDetailedInfoHeader(info);

	auto& tr = info.tp->detailedImGuiTraceIt->second.tr;
	TrReadContextScope scope{tr};
	tr_tick activeTick = info.activeTick;

	auto& entCache = tr.GetEntityCache();
	auto& entMap = entCache.GetEnts(activeTick);

	static std::vector<std::pair<TrIdx<TrEnt>, TrIdx<TrEntTransform>>> sortedEnts;
	sortedEnts.resize(entMap.size());
	std::ranges::transform(entMap, sortedEnts.begin(), std::identity{});
	std::ranges::sort(sortedEnts, std::less{}, [](auto& p) { return p.first->handle.GetEntryIndex(); });

	ImGui::TextUnformatted(ICON_CI_FILTER " Filter:");
	ImGui::SameLine();

	static ImGuiTextFilter filter;
	filter.Draw("##filter");
	ImGui::SetItemTooltip(
	    "Filter usage:\n"
	    "  \"\"         display all lines\n"
	    "  \"xxx\"      display lines containing \"xxx\"\n"
	    "  \"xxx,yyy\"  display lines containing \"xxx\" or \"yyy\"\n"
	    "  \"-xxx\"     hide lines containing \"xxx\"");

	std::bitset<MAX_EDICTS> filterPassedSet{};
	int nPassed = 0;

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

	ImGui::Text("showing %d/%u entities", nPassed, sortedEnts.size());

	for (auto [entIdx, entTransIdx] : sortedEnts)
	{
		const TrEnt& ent = **entIdx;

		if (!filterPassedSet[ent.handle.GetEntryIndex()])
			continue;

		ImGui::PushID(ent.handle.GetEntryIndex());
		const char* entName = *ent.nameIdx;
		ImGui::BeginGroup();
		if (ImGui::TreeNodeEx("ent_entry",
		                      ImGuiTreeNodeFlags_SpanFullWidth,
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
					ImGui::Text("OBB mins: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(mins));
					ImGui::Text("OBB maxs: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(maxs));
					ImGui::Text("server pos: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(pos));
					ImGui::Text("server ang: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(ang));
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
				                           "%u physics object%s",
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
							ImGui::Text("pos: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(pos));
							ImGui::Text("ang: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(ang));

							if (objMesh.ballRadius > 0)
							{
								ImGui::Text("ball mesh of radius %f",
								            objMesh.ballRadius);
							}
							else
							{
								ImGui::Text("mesh with %u verts", objMesh.vertIdxSp.n);
							}
							SptImGui::EndBordered();
						}
						ImGui::PopID();
					}
					ImGui::TreePop();
				}
				SptImGui::EndBordered();
				// pad the remaining space with a dummy for hover detection
				auto [_, height] = ImGui::GetItemRectSize();
				ImGui::SameLine();
				ImGui::Dummy(ImVec2{ImGui::GetContentRegionAvail().x, height});
			}
			ImGui::TreePop();
		}
		ImGui::EndGroup();

		if (ImGui::IsItemHovered())
			entCache.hoveredEnt = entIdx;

		ImGui::PopID();
	}
}

#endif