#include "stdafx.hpp"

#include "tr_imgui.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "spt/features/visualizations/imgui/imgui_interface.hpp"
#include "thirdparty/imgui/imgui_stdlib.h"

using namespace player_trace;

/*
* Iterate over traces groups and entries as we draw them, handle drag/drop behavior along the way.
* The drag/drop behavior is currently pretty gross because of imgui API limitations - normally you
* drag over an imgui *item*. In this case I want the whole window to act as a target and manually
* reorder the internal linked lists. This means this code must manually track positions of items,
* which looks pretty gross.
*/
class GroupTintCallback
{
	TrTracePlayer& tracePlayer;
	ImGuiSelectionBasicStorage& imguiEntrySelection;
	inline static std::vector<ImGuiID> selectionList;

	// these are set as we iterate over groups and the entries in them
	TrTracePlayer::group_handle groupHandle{};
	TrTracePlayer::group_entry_handle groupEntryHandle{};

	static constexpr const char* GROUP_DRAG_DROP_ID = "TRACE_GROUP_DD";
	static constexpr const char* ENTRY_DRAG_DROP_ID = "TRACE_GROUP_ENTRIES_DD";

	static constexpr const char* VIS_LABEL_ON = ICON_CI_EYE "###vis";
	static constexpr const char* VIS_LABEL_OFF = ICON_CI_EYE_CLOSED "###vis";
	static constexpr const char* SOLO_LABEL_ON = ICON_CI_STAR_FULL "###solo";
	static constexpr const char* SOLO_LABEL_OFF = ICON_CI_STAR "###solo";

	struct
	{
		bool superSoloHoveredTrace = true;
	} inline static imguiPersist;

public:
	GroupTintCallback(TrTracePlayer& tracePlayer)
	    : tracePlayer(tracePlayer), imguiEntrySelection(tracePlayer.imguiEntrySelect)
	{
#ifndef NDEBUG
		// sanity check
		for (auto& group : tracePlayer.Groups())
			for (auto& entry : group.entries)
				Assert(&*entry->second.group == &group && &*entry->second.groupPos == &entry);
#endif
		selectionList.clear();
		selectionList.reserve(tracePlayer.Traces().size());
		imguiEntrySelection.UserData = &selectionList;
		imguiEntrySelection.AdapterIndexToStorageId = [](ImGuiSelectionBasicStorage* self, int idx)
		{ return (*((std::vector<ImGuiID>*)self->UserData))[idx]; };
	}

	~GroupTintCallback()
	{
		imguiEntrySelection.UserData = nullptr;
		imguiEntrySelection.AdapterIndexToStorageId = nullptr;
	}

	void Draw()
	{
		SptImGui::HelpMarker(
		    "You can organize traces into color-coded groups.\n"
		    "After importing/recording your traces, create groups\n"
		    "and drag/drop traces into them (you can drag/drop\n"
		    "multiple traces at a time with ctrl/shift click).\n"
		    "The names of the groups can be called whatever you\n"
		    "want - they are just for organization.");

		ImGui::Checkbox("Solo hovered trace", &imguiPersist.superSoloHoveredTrace);
		ImGui::SameLine();
		SptImGui::HelpMarker(
		    "Only draw the trace that the mouse is hovered\n"
		    "over, ignoring solo/visible settings.");

		// top left corner of the group drag/drop target
		float curGroupDragDropMinY = ImGui::GetCursorScreenPos().y;
		ImVec2 groupItemBoundsX(-1, -1);

		// multi-select logic copied from imgui demo
		ImGuiMultiSelectIO* msIo = ImGui::BeginMultiSelect(ImGuiMultiSelectFlags_None,
		                                                   imguiEntrySelection.Size,
		                                                   tracePlayer.Traces().size());
		imguiEntrySelection.ApplyRequests(msIo);

		// iterate over each group
		auto& groups = tracePlayer.Groups();
		for (groupHandle = TrTracePlayer::group_handle::Begin(groups);
		     groupHandle != TrTracePlayer::group_handle::End(groups);)
		{
			auto& group = *groupHandle;
			ImGui::PushID(&group);

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
			               GroupDragDropTargetCallback,
			               this);

			ImGui::Indent();

			float curEntryDragDropMinY = curGroupItemMins.y;
			ImVec2 entryItemBoundsX(curGroupItemMins.x, curGroupItemMaxs.x);

			// iterate over each entry in the group
			for (groupEntryHandle = TrTracePlayer::group_entry_handle::Begin(group.entries);
			     groupEntryHandle != TrTracePlayer::group_entry_handle::End(group.entries);
			     ++groupEntryHandle)
			{
				auto& entryHandle = *groupEntryHandle;
				ImGui::PushID(&*entryHandle);
				DrawTraceEntry();
				ImVec2 curEntryItemMins = ImGui::GetItemRectMin();
				ImVec2 curEntryItemMaxs = ImGui::GetItemRectMax();
				entryItemBoundsX = {curEntryItemMins.x, curEntryItemMaxs.x};
				if (imguiEntrySelection.Contains(tracePlayer.HandleToMultiSelectId(entryHandle)))
					EntryDragDropSource();
				DragDropTarget(ENTRY_DRAG_DROP_ID,
				               curEntryDragDropMinY,
				               curEntryItemMins,
				               curEntryItemMaxs,
				               EntryDragDropTargetCallback,
				               this);
				ImGui::PopID();

				curEntryDragDropMinY = (curEntryItemMins.y + curEntryItemMaxs.y) * .5f;
			}

			float y = ImGui::GetCursorScreenPos().y;
			DragDropTarget(ENTRY_DRAG_DROP_ID,
			               curEntryDragDropMinY,
			               ImVec2(entryItemBoundsX.x, y),
			               ImVec2(entryItemBoundsX.y, y),
			               EntryDragDropTargetCallback,
			               this);

			ImGui::Unindent();

			ImGui::EndGroup();

			// next drag drop source will start in the middle of the group item
			curGroupDragDropMinY = (curGroupItemMins.y + curGroupItemMaxs.y) * .5f;

			auto prev = groupHandle++;
			if (deleteThisGroup)
				tracePlayer.Remove(prev);

			ImGui::PopID();
		}

		msIo = ImGui::EndMultiSelect();
		imguiEntrySelection.ApplyRequests(msIo);

		float y = ImGui::GetCursorScreenPos().y;
		DragDropTarget(GROUP_DRAG_DROP_ID,
		               curGroupDragDropMinY,
		               ImVec2(groupItemBoundsX.x, y),
		               ImVec2(groupItemBoundsX.y, y),
		               GroupDragDropTargetCallback,
		               this);

		// needed for the target cursor readjusting hack
		ImGui::Dummy(ImVec2(0, 0));

		if (ImGui::SmallButton(ICON_CI_PLUS))
		{
			color32 c(std::rand(), std::rand(), std::rand(), 255);
			tracePlayer.AddGroup(c);
		}
		ImGui::SetItemTooltip("add new group");
	}

private:
	// dragging groups is easy - just reorder them
	static void GroupDragDropTargetCallback(const ImGuiPayload* payload, void* userData)
	{
		auto thisptr = ((GroupTintCallback*)userData);
		thisptr->tracePlayer.ReorderGroup(*(TrTracePlayer::group_handle*)payload->Data, thisptr->groupHandle);
	}

	// dragging entries is more complicated - iterate over each selected entry and splice it into the group's list
	static void EntryDragDropTargetCallback(const ImGuiPayload* payload, void* userData)
	{
		auto thisptr = ((GroupTintCallback*)userData);
		auto toGroup = thisptr->groupHandle;
		auto toGroupPos = thisptr->groupEntryHandle;

		void* imguiIt = NULL;
		ImGuiID id;
		while (thisptr->imguiEntrySelection.GetNextSelectedItem(&imguiIt, &id))
		{
			auto pTraceEntry = thisptr->tracePlayer.MultiSelectIdToPtr(id);
			const TrTracePlayer::TraceEntry& entry = pTraceEntry->second;
			auto fromEntryHandle = *entry.groupPos;

			/*
			* When using drag/drop for multiple entries across multiple groups, the entries
			* sometimes get reordered. I'm setting PreserveOrder on the ImGui multi-select but
			* that seems to preserve select order, not submit order. Probably fine :).
			*/

			if (toGroup == entry.group && toGroupPos == entry.groupPos)
				++toGroupPos; // without this if you try to drop entries into the slot they're in they'll get reversed
			else
				thisptr->tracePlayer.MoveTraceToGroup(fromEntryHandle, toGroup, toGroupPos);
		}
	}

	void DrawGroupItem(bool& deleteThisGroup)
	{
		auto& group = *groupHandle;

		SptImGui::BeginBordered();
		color32 tintCol = group.GetTint();
		ImGuiColorEditFlags colorEditFlags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel
		                                     | ImGuiColorEditFlags_AlphaPreviewHalf
		                                     | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_AlphaBar;
		if (SptImGui::Color32Edit("group tint", tintCol, colorEditFlags))
			group.SetTint(tintCol);

		ImGui::SameLine();

		bool isDefault = groupHandle == tracePlayer.GetDefaultGroup();
		ImGui::BeginDisabled(isDefault);
		ImGuiInputTextFlags groupNameFlags = ImGuiInputFlags_None;
		if (isDefault)
			groupNameFlags |= ImGuiInputTextFlags_ReadOnly;
		ImGui::InputTextWithHint("##group_name", "enter group name", &group.name, groupNameFlags);
		if (isDefault)
			ImGui::SetItemTooltip("default group can't be renamed");
		ImGui::EndDisabled();

		ImGui::SameLine();
		TrTracePlayer::BoolOptInfo visibleOptInfo = tracePlayer.GetGroupVisibleOpt(groupHandle);
		ImGui::BeginDisabled(!visibleOptInfo.allowUiChange);
		if (ImGui::SmallButton(visibleOptInfo.enabled ? VIS_LABEL_ON : VIS_LABEL_OFF))
			tracePlayer.SetGroupVisibleOpt(groupHandle, !visibleOptInfo.enabled);
		ImGui::SetItemTooltip("toggle group visibility");
		ImGui::EndDisabled();

		ImGui::SameLine();
		TrTracePlayer::BoolOptInfo soloOptInfo = tracePlayer.GetGroupSoloOpt(groupHandle);
		ImGui::BeginDisabled(!soloOptInfo.allowUiChange);
		if (ImGui::SmallButton(soloOptInfo.enabled ? SOLO_LABEL_ON : SOLO_LABEL_OFF))
			tracePlayer.SetGroupSoloOpt(groupHandle, !soloOptInfo.enabled);
		ImGui::SetItemTooltip("solo group");
		ImGui::EndDisabled();

		if (!isDefault)
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
			ImGui::SetDragDropPayload(GROUP_DRAG_DROP_ID, &groupHandle, sizeof groupHandle);
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

		auto entryHandle = *groupEntryHandle;
		size_t entryIdx = selectionList.size();
		ImGuiID entrySelectionId = tracePlayer.HandleToMultiSelectId(entryHandle);
		selectionList.push_back(entrySelectionId);

		auto& [path, entry] = *entryHandle;
		bool selected = imguiEntrySelection.Contains(entrySelectionId);
		ImGui::SetNextItemSelectionUserData(entryIdx);
		ImGui::SetNextItemAllowOverlap();
		ImGui::Selectable("##dummy", selected);
		ImGui::SameLine();
		ImGui::TextUnformatted(utils::GetPathProximateToModDir(path).string().c_str());

		// TODO account for spt_trace_draw_while_recording somewhere

		ImGui::SameLine();
		TrTracePlayer::BoolOptInfo visibleOptInfo = tracePlayer.GetTraceVisibleOpt(entryHandle);
		ImGui::BeginDisabled(!visibleOptInfo.allowUiChange);
		if (ImGui::SmallButton(visibleOptInfo.enabled ? VIS_LABEL_ON : VIS_LABEL_OFF))
			tracePlayer.SetTraceVisibleOpt(entryHandle, !visibleOptInfo.enabled);
		ImGui::SetItemTooltip("toggle trace visibility");
		ImGui::EndDisabled();

		ImGui::SameLine();
		TrTracePlayer::BoolOptInfo soloOptInfo = tracePlayer.GetTraceSoloOpt(entryHandle);
		ImGui::BeginDisabled(!soloOptInfo.allowUiChange);
		if (ImGui::SmallButton(soloOptInfo.enabled ? SOLO_LABEL_ON : SOLO_LABEL_OFF))
			tracePlayer.SetTraceSoloOpt(entryHandle, !soloOptInfo.enabled);
		ImGui::SetItemTooltip("solo trace");
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 0));

		ImGui::EndGroup();

		if (imguiPersist.superSoloHoveredTrace && ImGui::IsItemHovered())
			tracePlayer.imguiHoveredTrace = entryHandle;
	}

	void EntryDragDropSource()
	{
		// imgui groups apparently also have a null ID
		ImGuiDragDropFlags sourceDdFlags =
		    ImGuiDragDropFlags_SourceAllowNullID | ImGuiDragDropFlags_PayloadAutoExpire;
		if (ImGui::BeginDragDropSource(sourceDdFlags))
		{
			ImGui::Text("Move %d trace(s)", imguiEntrySelection.Size);
			ImGui::SetDragDropPayload(ENTRY_DRAG_DROP_ID, nullptr, 0);
			ImGui::EndDragDropSource();
		}
	}
};

void tr_imgui::RenderStyleTab()
{
	auto& tp = TrTracePlayer::Singleton();

	if (ImGui::TreeNode("Trace groups"))
	{
		GroupTintCallback groupTints(tp);
		groupTints.Draw();
		ImGui::TreePop();
	}

	ImGui::Text("Drawing %u trace%s", tp.nDrawnTracesLastFrame, tp.nDrawnTracesLastFrame == 1 ? "" : "s");

	ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
	if (ImGui::TreeNode("Multiple trace options"))
	{

	}
}

#endif
