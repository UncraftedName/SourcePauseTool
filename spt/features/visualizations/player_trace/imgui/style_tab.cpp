#include "stdafx.hpp"

#include "tr_imgui.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "spt/features/visualizations/imgui/imgui_interface.hpp"
#include "thirdparty/imgui/imgui_stdlib.h"

using namespace player_trace;

/*
* Terminology:
* - group: (usually) a tint group in the trace player
* - entry: an element in a tint group
*/
class GroupTintCallback
{
	TrTracePlayer& tracePlayer;
	ImGuiSelectionBasicStorage& entrySelection;
	inline static std::vector<ImGuiID> selectionList;

	TrTracePlayer::group_it groupIt{};                // current group
	TrTracePlayer::group_entries::iterator entryIt{}; // current entry in group

	static constexpr const char* GROUP_DRAG_DROP_ID = "TRACE_GROUP_DD";
	static constexpr const char* ENTRY_DRAG_DROP_ID = "TRACE_GROUP_ENTRIES_DD";

	static constexpr const char* VIS_LABEL_ON = ICON_CI_EYE "###vis";
	static constexpr const char* VIS_LABEL_OFF = ICON_CI_EYE_CLOSED "###vis";
	static constexpr const char* SOLO_LABEL_ON = ICON_CI_STAR_FULL "###solo";
	static constexpr const char* SOLO_LABEL_OFF = ICON_CI_STAR "###solo";

public:
	GroupTintCallback(TrTracePlayer& tracePlayer)
	    : tracePlayer(tracePlayer), entrySelection(tracePlayer.imguiEntrySelect)
	{
#ifndef NDEBUG
		// sanity check
		for (auto& group : tracePlayer.traceGroups)
		{
			for (auto& traceIt : group.entries)
			{
				Assert(&*traceIt->second.groupIt == &group
				       && &*traceIt->second.entryInGroupIt == &traceIt);
			}
		}
#endif
		selectionList.clear();
		selectionList.reserve(tracePlayer.AllTraces().size());
		entrySelection.UserData = &selectionList;
		entrySelection.AdapterIndexToStorageId = [](ImGuiSelectionBasicStorage* self, int idx)
		{ return (*((std::vector<ImGuiID>*)self->UserData))[idx]; };
	}

	~GroupTintCallback()
	{
		entrySelection.UserData = nullptr;
		entrySelection.AdapterIndexToStorageId = nullptr;
	}

	void Draw()
	{
		auto groupDragDropTargetFn = [](const ImGuiPayload* payload, void* userData)
		{
			auto thisptr = ((GroupTintCallback*)userData);
			auto& groups = thisptr->tracePlayer.traceGroups;
			auto fromIt = (TrTracePlayer::group_it*)payload->Data;
			groups.splice(thisptr->groupIt, groups, *fromIt);
		};

		auto entryDragDropTargetFn = [](const ImGuiPayload* payload, void* userData)
		{
			auto thisptr = ((GroupTintCallback*)userData);
			auto groupIt = thisptr->groupIt;
			auto targetEntryItTo = thisptr->entryIt;

			void* imguiIt = NULL;
			ImGuiID id;
			while (thisptr->entrySelection.GetNextSelectedItem(&imguiIt, &id))
			{
				auto pTraceEntry = (TrTracePlayer::trace_map::pointer)id;
				TrTracePlayer::Entry& entry = pTraceEntry->second;

				if (groupIt == entry.groupIt && targetEntryItTo == entry.entryInGroupIt)
				{
					// without this if you try to drop entries into the slot they're in they'll get reversed
					++targetEntryItTo;
				}
				else
				{
					groupIt->entries.splice(targetEntryItTo,
					                        entry.groupIt->entries,
					                        entry.entryInGroupIt);
					entry.groupIt = thisptr->groupIt;
				}
			}
		};

		// top left corner of the group drag/drop target
		float curGroupDragDropMinY = ImGui::GetCursorScreenPos().y;
		ImVec2 groupItemBoundsX(-1, -1);

		ImGuiMultiSelectIO* msIo = ImGui::BeginMultiSelect(ImGuiMultiSelectFlags_None,
		                                                   entrySelection.Size,
		                                                   tracePlayer.AllTraces().size());
		entrySelection.ApplyRequests(msIo);

		for (groupIt = tracePlayer.traceGroups.begin(); groupIt != tracePlayer.traceGroups.end();)
		{
			auto& group = *groupIt;
			ImGui::PushID(&*groupIt);

			bool deleteThisGroup = false;

			ImGui::BeginGroup();

			DrawGroupItem(deleteThisGroup);
			ImVec2 curGroupItemMins = ImGui::GetItemRectMin();
			ImVec2 curGroupItemMaxs = ImGui::GetItemRectMax();
			groupItemBoundsX = {curGroupItemMins.x, curGroupItemMaxs.x};
			GroupDragDropSource();
			DragDropTarget(GROUP_DRAG_DROP_ID,
			               curGroupDragDropMinY,
			               curGroupItemMins,
			               curGroupItemMaxs,
			               groupDragDropTargetFn,
			               this);

			ImGui::Indent();

			float curEntryDragDropMinY = curGroupItemMins.y;
			ImVec2 entryItemBoundsX(curGroupItemMins.x, curGroupItemMaxs.x);

			for (entryIt = group.entries.begin(); entryIt != group.entries.end(); ++entryIt)
			{
				auto& traceIt = *entryIt;
				ImGui::PushID(&*traceIt);
				DrawTraceEntry();
				ImVec2 curEntryItemMins = ImGui::GetItemRectMin();
				ImVec2 curEntryItemMaxs = ImGui::GetItemRectMax();
				entryItemBoundsX = {curEntryItemMins.x, curEntryItemMaxs.x};
				if (entrySelection.Contains((ImGuiID)(&*traceIt)))
					EntryDragDropSource();
				DragDropTarget(ENTRY_DRAG_DROP_ID,
				               curEntryDragDropMinY,
				               curEntryItemMins,
				               curEntryItemMaxs,
				               entryDragDropTargetFn,
				               this);
				ImGui::PopID();

				curEntryDragDropMinY = (curEntryItemMins.y + curEntryItemMaxs.y) * .5f;
			}

			float y = ImGui::GetCursorScreenPos().y;
			DragDropTarget(ENTRY_DRAG_DROP_ID,
			               curEntryDragDropMinY,
			               ImVec2(entryItemBoundsX.x, y),
			               ImVec2(entryItemBoundsX.y, y),
			               entryDragDropTargetFn,
			               this);

			ImGui::Unindent();

			ImGui::EndGroup();

			// next drag drop source will start in the middle of the group item
			curGroupDragDropMinY = (curGroupItemMins.y + curGroupItemMaxs.y) * .5f;

			auto prev = groupIt++;
			if (deleteThisGroup)
				tracePlayer.DeleteGroup(prev);

			ImGui::PopID();
		}

		msIo = ImGui::EndMultiSelect();
		entrySelection.ApplyRequests(msIo);

		float y = ImGui::GetCursorScreenPos().y;
		DragDropTarget(GROUP_DRAG_DROP_ID,
		               curGroupDragDropMinY,
		               ImVec2(groupItemBoundsX.x, y),
		               ImVec2(groupItemBoundsX.y, y),
		               groupDragDropTargetFn,
		               this);

		// needed for the target cursor readjusting hack
		ImGui::Dummy(ImVec2(0, 0));

		if (ImGui::SmallButton(ICON_CI_PLUS))
		{
			color32 c(std::rand(), std::rand(), std::rand(), 255);
			tracePlayer.AddCustomGroup(c);
		}
		ImGui::SetItemTooltip("add new group");
	}

private:
	void DrawGroupItem(bool& deleteThisGroup)
	{
		auto& group = *groupIt;

		SptImGui::BeginBordered();
		color32 tintCol = group.GetTint();
		ImGuiColorEditFlags colorEditFlags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel
		                                     | ImGuiColorEditFlags_AlphaPreviewHalf
		                                     | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_AlphaBar;
		if (SptImGui::Color32Edit("group tint", tintCol, colorEditFlags))
			group.SetTint(tintCol);

		ImGui::SameLine();

		ImGui::BeginDisabled(group.isDefault);
		ImGuiInputTextFlags groupNameFlags = ImGuiInputFlags_None;
		if (groupIt == tracePlayer.defaultGroupIt)
			groupNameFlags |= ImGuiInputTextFlags_ReadOnly;
		ImGui::InputTextWithHint("##group_name", "enter group name", &group.name, groupNameFlags);
		if (group.isDefault)
			ImGui::SetItemTooltip("default group can't be renamed");
		ImGui::EndDisabled();

		ImGui::SameLine();
		TrTracePlayer::BoolOptInfo visibleOptInfo = tracePlayer.GroupVisibleOptInfo(groupIt);
		ImGui::BeginDisabled(!visibleOptInfo.allowUiChange);
		if (ImGui::SmallButton(visibleOptInfo.enabled ? VIS_LABEL_ON : VIS_LABEL_OFF))
			tracePlayer.GroupVisibleOptSet(groupIt, !visibleOptInfo.enabled);
		ImGui::SetItemTooltip("toggle group visibility");
		ImGui::EndDisabled();

		ImGui::SameLine();
		TrTracePlayer::BoolOptInfo soloOptInfo = tracePlayer.GroupSoloOptInfo(groupIt);
		ImGui::BeginDisabled(!soloOptInfo.allowUiChange);
		if (ImGui::SmallButton(soloOptInfo.enabled ? SOLO_LABEL_ON : SOLO_LABEL_OFF))
			tracePlayer.GroupSoloOptSet(groupIt, !soloOptInfo.enabled);
		ImGui::SetItemTooltip("solo group");
		ImGui::EndDisabled();

		if (!group.isDefault)
		{
			ImGui::SameLine();
			deleteThisGroup = ImGui::SmallButton(ICON_CI_TRASH);
			ImGui::SetItemTooltip("delete group (all entries move to the default group)");
		}

		// TODO change the entries to be an invisible table?

		SptImGui::EndBordered();
	}

	void GroupDragDropSource()
	{
		// SptImGui::Bordered uses a table which apparently has a null ID
		ImGuiDragDropFlags sourceDdFlags =
		    ImGuiDragDropFlags_SourceAllowNullID | ImGuiDragDropFlags_PayloadAutoExpire;
		if (ImGui::BeginDragDropSource(sourceDdFlags))
		{
			ImGui::TextUnformatted("Reorder group list");
			ImGui::SetDragDropPayload(GROUP_DRAG_DROP_ID, &groupIt, sizeof groupIt);
			ImGui::EndDragDropSource();
		}
	}

	using DragDropTargetFn = void (*)(const ImGuiPayload* payload, void* userData);

	void DragDropTarget(const char* type,
	                    float curItemDragDropMinY,
	                    ImVec2 itemMins,
	                    ImVec2 itemMaxs,
	                    DragDropTargetFn deliverFn,
	                    void* userData)
	{
		/*
		* TODO use BeginDragDropTargetCustom or BeginDragDropTargetViewport?
		* That would remove:
		* - dummy
		* - cursor readjusting
		* - possibly allow extending the drop target to the top/bottom of the window
		* - remove the need for a 0-size dummy at the end
		* - the additional dummy after the last group
		*/
		Vector2D tmpOldCursorPos = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos({0, curItemDragDropMinY});
		ImGui::Dummy({100000.f, (itemMins.y + itemMaxs.y) * .5f - curItemDragDropMinY});
		if (ImGui::BeginDragDropTarget())
		{
			ImGuiDragDropFlags targetDdFlags =
			    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
			const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type, targetDdFlags);
			if (payload && payload->Delivery)
			{
				deliverFn(payload, userData);
			}
			else if (payload)
			{
				// draw target bar (instead of default rect)
				float y = itemMins.y - ImGui::GetStyle().ItemSpacing.y * .5f;
				ImGui::GetWindowDrawList()->AddLine(ImVec2(itemMins.x, y),
				                                    ImVec2(itemMaxs.x, y),
				                                    ImGui::GetColorU32(ImGuiCol_DragDropTarget),
				                                    2.f);
			}
			ImGui::EndDragDropTarget();
		}
		ImGui::SetCursorScreenPos(tmpOldCursorPos - ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 2.f));
		// see ImGui::ErrorCheckUsingSetCursorPosToExtendParentBoundaries
		ImGui::Dummy(ImVec2(0, 0));
	}

	void DrawTraceEntry()
	{
		ImGui::BeginGroup();

		auto& traceIt = *entryIt;
		size_t entryIdx = selectionList.size();
		ImGuiID entrySelectionId = (ImGuiID)(&*traceIt);
		selectionList.push_back(entrySelectionId);

		auto& [path, entry] = *traceIt;
		bool selected = entrySelection.Contains(entrySelectionId);
		ImGui::SetNextItemSelectionUserData(entryIdx);
		ImGui::SetNextItemAllowOverlap();
		ImGui::Selectable("##dummy", selected);
		ImGui::SameLine();
		ImGui::TextUnformatted(utils::GetPathProximateToModDir(path).string().c_str());

		// TODO account for spt_trace_draw_while_recording somewhere

		ImGui::SameLine();
		TrTracePlayer::BoolOptInfo visibleOptInfo = tracePlayer.TraceVisibleOptInfo(traceIt);
		ImGui::BeginDisabled(!visibleOptInfo.allowUiChange);
		if (ImGui::SmallButton(visibleOptInfo.enabled ? VIS_LABEL_ON : VIS_LABEL_OFF))
			tracePlayer.TraceVisibleOptSet(traceIt, !visibleOptInfo.enabled);
		ImGui::SetItemTooltip("toggle trace visibility");
		ImGui::EndDisabled();

		ImGui::SameLine();
		TrTracePlayer::BoolOptInfo soloOptInfo = tracePlayer.TraceSoloOptInfo(traceIt);
		ImGui::BeginDisabled(!soloOptInfo.allowUiChange);
		if (ImGui::SmallButton(soloOptInfo.enabled ? SOLO_LABEL_ON : SOLO_LABEL_OFF))
			tracePlayer.TraceSoloOptSet(traceIt, !soloOptInfo.enabled);
		ImGui::SetItemTooltip("solo trace");
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 0));

		ImGui::EndGroup();

		// TODO inform the user that hovering selects a specific trace! at least make a checkbox!
		if (ImGui::IsItemHovered())
			tracePlayer.imguiHoveredTraceIt = traceIt;
	}

	void EntryDragDropSource()
	{
		// imgui groups apparently also have a null ID
		ImGuiDragDropFlags sourceDdFlags =
		    ImGuiDragDropFlags_SourceAllowNullID | ImGuiDragDropFlags_PayloadAutoExpire;
		if (ImGui::BeginDragDropSource(sourceDdFlags))
		{
			ImGui::Text("Move %d trace(s)", entrySelection.Size);
			ImGui::SetDragDropPayload(ENTRY_DRAG_DROP_ID, nullptr, 0);
			ImGui::EndDragDropSource();
		}
	}
};

void tr_imgui::RenderStyleTab()
{
	if (ImGui::TreeNode("Trace groups"))
	{
		GroupTintCallback cb(TrTracePlayer::Singleton());
		cb.Draw();
		ImGui::TreePop();
	}
}

#endif
