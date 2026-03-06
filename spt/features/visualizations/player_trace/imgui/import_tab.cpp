#include "stdafx.hpp"

#include "tr_imgui.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "spt/features/visualizations/imgui/imgui_interface.hpp"
#include "thirdparty/imgui/imgui_stdlib.h"

using namespace player_trace;

constexpr const char* TR_FILE_SELECT_WND_ID = "trace_file_select";

static std::string g_importErrStr;
static SptImGui::TimedToolTip g_importErrTip;

void tr_imgui::TraceFileSelectionTabCallback(std::unique_ptr<ImGuiFileDialog>& igfd)
{
	auto& tp = TrTracePlayer::Singleton();
	auto& traces = tp.AllTraces();

	ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
	if (ImGui::TreeNode("Loaded traces"))
	{
		/*ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
	ImGui::SliderAngle("style.TableAngledHeadersAngle", &ImGui::GetStyle().TableAngledHeadersAngle, -50.0f, +50.0f);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
	ImGui::SliderFloat2("style.TableAngledHeadersTextAlign",
	                    (float*)&ImGui::GetStyle().TableAngledHeadersTextAlign,
	                    0.0f,
	                    1.0f,
	                    "%.2f");*/

		if (!traces.empty())
		{
			ImGuiTableFlags table_flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders
			                              | ImGuiTableFlags_HighlightHoveredColumn;
			ImGuiTableColumnFlags column_flags = ImGuiTableColumnFlags_AngledHeader
			                                     | ImGuiTableColumnFlags_WidthFixed
			                                     | ImGuiTableColumnFlags_NoHeaderWidth;

			if (ImGui::BeginTable("trace_table", 4, table_flags))
			{
				ImGui::TableSetupColumn("path", column_flags);
				ImGui::TableSetupColumn("first map", column_flags);
				ImGui::TableSetupColumn("tick length", column_flags);
				ImGui::TableSetupColumn("delete", column_flags);
				ImGui::TableSetupScrollFreeze(0, 2);

				// TODO special highlighting for currently recording trace, and selected for imgui trace

				ImGui::TableAngledHeadersRow();
				ImGui::TableHeadersRow();
				int i = 0;
				auto itToDelete = traces.end();
				for (auto it = traces.begin(); it != traces.end(); ++it)
				{
					auto& [path, ev] = *it;

					TrReadContextScope scope{ev.tr};

					ImGui::PushID(i++);
					ImGui::TableNextRow();

					// path
					ImGui::TableSetColumnIndex(0);
					ImGui::AlignTextToFramePadding();
					ImGui::Text("%s", utils::GetPathProximateToModDir(path).string().c_str());

					// first map
					ImGui::TableSetColumnIndex(1);
					TrIdx<TrMap> mapIdx = ev.tr.GetMapAtTick(0);
					TrStr mapNameIdx = mapIdx.IsValid() ? mapIdx->nameIdx : TrStr{};
					ImGui::Text("%s", mapNameIdx.IsValid() ? *mapNameIdx : "<NULL>");

					// tick length
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%u", ev.tr.numRecordedTicks);

					// delete button
					ImGui::TableSetColumnIndex(3);
					if (SptImGui::SmallIconButton(ICON_CI_TRASH))
						itToDelete = it;
					ImGui::PopID();
				}

				if (itToDelete != traces.end())
					tp.Remove(itToDelete);

				ImGui::EndTable();
			}

			if (ImGui::Button("Clear all traces"))
				tp.Clear(false);
		}
		else
		{
			ImGui::TextUnformatted("No loaded traces");
		}

		ImGui::TreePop();
	}

	if (ImGui::Button(ICON_CI_NEW_FILE " Import traces"))
	{
		if (!igfd)
		{
			igfd = std::make_unique<ImGuiFileDialog>();
			// TODO make these colors global somewhere?
			static ImVec4 traceFileColor = ImVec4(0.2f, 1.0f, .9f, 1.f);
			igfd->SetFileStyle(IGFD_FileStyleByTypeDir, "", ImVec4(1.f, 1.f, 1.f, 1.f), ICON_CI_FOLDER);
			igfd->SetFileStyle(IGFD_FileStyleByTypeFile, "", traceFileColor, ICON_CI_FILE);

			// TODO IGFD_FileStyleByTypeLink
			// igfd->SetFileStyle(IGFD_FileStyleByTypeLink, "", ImVec4(0.2f, 1.0f, .9f, 1.f), ICON_CI_FILE);

			// NOTE: capturing the TrTracePlayer by reference for the lifetime of the ImGuiFileDialog
			igfd->SetFileStyle(
			    [](const IGFD::FileInfos& info, IGFD::FileStyle& outStyle) -> bool
			    {
				    if (!info.fileType.isFile())
					    return false;
				    std::filesystem::path fullPath =
				        std::filesystem::path(info.filePath) / info.fileNameExt;
				    if (TrTracePlayer::Singleton().AllTraces().contains(fullPath))
				    {
					    outStyle.color = {
					        traceFileColor.x * 0.5f,
					        traceFileColor.y * 0.5f,
					        traceFileColor.z * 0.5f,
					        traceFileColor.w,
					    };
					    outStyle.flags = IGFD_FileStyleByFullName;
					    return true;
				    }
				    return false;
			    });
		}
		IGFD::FileDialogConfig cfg{
		    .path = GetGameDir(),
		    .countSelectionMax = 0,
		    .flags = ImGuiFileDialogFlags_NaturalSorting | ImGuiFileDialogFlags_Modal
		             | ImGuiFileDialogFlags_DisableCreateDirectoryButton
		             | ImGuiFileDialogFlags_DisableThumbnailMode | ImGuiFileDialogFlags_ShowDevicesButton,
		    .userFileAttributes = [](IGFD::FileInfos* info, IGFD::UserDatas) -> bool
		    {
			    // still show the the files if they're already loaded, but let the user know
			    if (!info)
				    return true;
			    if (!info->fileType.isFile())
				    return true;
			    std::filesystem::path fullPath = std::filesystem::path(info->filePath) / info->fileNameExt;
			    if (TrTracePlayer::Singleton().AllTraces().contains(fullPath))
			    {
				    info->tooltipMessage = "Already loaded";
				    // TODO this should work if tooltipColumn==-1, submit a bug
				    info->tooltipColumn = 0;
				    return true;
			    }
			    return true;
		    },
		};

		// TODO move this button into the tree node?
		// TODO remember last used path?
		igfd->OpenDialog(TR_FILE_SELECT_WND_ID, "Select traces", TrTracePlayer::COMPRESSED_FILE_EXT, cfg);
	}

	// g_importErrTip.Show(SPT_IMGUI_WARN_COLOR_YELLOW, 5.);
}

void tr_imgui::TraceFileSelectionWindowCallback(std::unique_ptr<ImGuiFileDialog>& igfd)
{
	if (!igfd || !igfd->IsOpened(TR_FILE_SELECT_WND_ID))
		return;

	auto& tp = TrTracePlayer::Singleton();
	Vector2D viewPortSize = ImGui::GetMainViewport()->Size;

	if (igfd->Display(TR_FILE_SELECT_WND_ID, ImGuiWindowFlags_NoCollapse, viewPortSize * 0.25f))
	{
		if (igfd->IsOk())
		{
			g_importErrTip.StopShowing();
			g_importErrStr.clear();
			for (auto& [_, pathStr] : igfd->GetSelection())
			{
				ser::StatusTracker status;
				tp.TryLoadFromDisk(pathStr, status);
				if (!status.Ok())
				{
					if (g_importErrStr.empty())
						g_importErrStr = "Import error(s):";
					g_importErrStr += std::format("\n{}: {}", pathStr, status.GetStatus().errMsg);
				}
			}
			if (!g_importErrStr.empty())
				g_importErrTip.StartShowing(g_importErrStr.c_str());
		}
		igfd->Close();
	}
}

bool tr_imgui::DrawDetailedTraceSelect()
{
	auto& tp = TrTracePlayer::Singleton();
	auto& traces = tp.AllTraces();
	auto& it = tp.detailedImGuiTraceIt;

	if (it == traces.end())
		it = traces.begin();

	bool anyLoaded = it != traces.end();
	ImGui::BeginDisabled(!anyLoaded);

	if (ImGui::BeginCombo("Trace selection",
	                      anyLoaded ? utils::GetPathProximateToModDir(it->first).string().c_str()
	                                : "no loaded traces"))
	{
		for (auto newIt = traces.begin(); newIt != traces.end(); ++newIt)
		{
			bool isSelected = newIt == it;
			if (ImGui::Selectable(utils::GetPathProximateToModDir(newIt->first).string().c_str(),
			                      &isSelected))
			{
				it = newIt;
			}
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::EndDisabled();

	return anyLoaded;
}

#endif
