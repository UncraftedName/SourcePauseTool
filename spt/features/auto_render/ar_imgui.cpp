#include "stdafx.hpp"

#include "ar_interface.hpp"
#include "ar_placeholders.hpp"
#include "ar_util.hpp"
#include "spt/features/visualizations/imgui/imgui_interface.hpp"
#include "spt/utils/interfaces.hpp"
#include "thirdparty/imgui/imgui_stdlib.h"
#include "thirdparty/imgui/ImGuiFileDialog/ImGuiFileDialog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#pragma comment(lib, "Comdlg32.lib")

#include <set>

#undef clamp

// TODO - move to public API so we can call this from a concmd
static std::string ArCreateDefaultCmdLine()
{
	return std::format(
	    "\"{0}\" -report -nostdin "
	    "-f nut -i \"{1}\" "
	    "-y -c:v libx264 -pix_fmt yuv420p -crf 18 "
	    "-c:a aac -b:a 192k "
	    "-shortest -preset veryfast \"{2}\\{3}{4}.mp4\"",
	    ArGlobalPlaceholders::EXE_PATH.key,
	    ArGlobalPlaceholders::PIPE_NAME.key,
	    ArGlobalPlaceholders::RENDER_WORKING_DIR.key,
	    ArGlobalPlaceholders::DATE_TIME.key,
	    ArGlobalPlaceholders::DEMO_NAME.key);
}

// a whole bunch of singleton ImGui state and small functions to render sections of the autorender tab
struct ArImGuiPersist
{
	std::optional<bool> lastFfmpegSearchSuccess;
	bool resetCmdLine = true;
	ArSyncMode syncMode = AR_SYNC_THREADED;
	int nFramesInFlight = 3;

	std::string cmdLineUnformatted;
	std::string cmdLineFormatted;
	std::vector<std::string> unrecognizedPlaceholders;

	struct DefaultCvar
	{
		ArCvarSetting cvarSetting;
		std::optional<std::string> helpText = std::nullopt; // if set, this is a mutable cvar
		// TODO add a checkbox for mutable cvars, force it to be on for non-mutables
		// TODO add export/import for these
	};

	std::vector<DefaultCvar> defaultCvarSettings{
	    DefaultCvar{{"host_framerate", "-1"}, "Required for video & audio syncing"},
	    DefaultCvar{{"fps_max", "-1"},
	                "Required for correct audio syncing for now (only ran when capturing audio)"},
	    DefaultCvar{{"snd_surround_speakers", "2"}, "Sound recording code requires stereo audio"},
	    DefaultCvar{{"snd_lockpartial", "0"}, "Required for sound recording"},
	    DefaultCvar{{"volume", "0"}, "Game will not output sound, but audio will still be recorded"},
	    DefaultCvar{{"gl_clear", "1"}},
	    DefaultCvar{{"spt_focus_nosleep", "1"}},
	    DefaultCvar{{"spt_disable_tone_map_reset", "1"}}, // TODO this should only be for multiple demos
	    DefaultCvar{{"spt_demo_block_cmd", "1"}},         // TODO this should only be for multiple demos
	    DefaultCvar{{"spt_override_tpose", "17"}},
	};

	std::vector<ArCvarSetting> userCvarSettings;

	float outputFramerate = 60.f;
	float playbackSpeed = 1.f;
	float volume = .5f;
	bool captureAudio = true;
	bool recordWhenConsoleIsOpen = false;
	bool recordAfterImGuiCallbacks = false;

	struct DemoSelector
	{
		constexpr static const char* FILE_DIALOG_ID = "ArDemoSelectDialogueKey";

		// full path, display path
		using demoFilePath = std::pair<std::filesystem::path, std::string>;

		struct DemoFileCmpNatural
		{
			bool operator()(const demoFilePath& a, const demoFilePath& b) const
			{
				/*
				* ImGuiFileDialog comes with IGFD::Utils::NaturalCompare for
				* ImGuiFileDialogFlags_NaturalSorting but it kinda sucks.
				* See: https://github.com/aiekick/ImGuiFileDialog/issues/223.
				*/
				return CompareStringEx(LOCALE_NAME_INVARIANT,
				                       NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
				                       a.first.c_str(),
				                       -1,
				                       b.first.c_str(),
				                       -1,
				                       NULL,
				                       NULL,
				                       0)
				       == CSTR_LESS_THAN;
			}
		};

		std::set<demoFilePath, DemoFileCmpNatural> paths;

	} demoSelector;

	void TabCallback();

	void DrawMultiDemoJobStatus(const ArRunningMultiDemoJobStatus* multiDemoJobStat,
	                            const ArRunningMovieJobStatus* curMovieJobStat);
	void DrawRunningJobStatus(const ArRunningMovieJobStatus* jobStat);
	void DrawLastFinishedJobStatus(const ArMovieJobResult* jobResult);

	std::unique_ptr<ArDeferredMovieJob> CreateDeferredJob();

	void DrawDemoPaths();
	bool DrawLogFolderButton(); // returns true if log folder was opened

	void DrawFfmpegPath();
	void DrawPipeNameOptions();
	void DrawFramerateOptions();
	void DrawAudioOptions();
	void DrawSyncMode();

	bool DrawUnformattedCmdLine(); // returns true if was updated
	void DrawDefaultPlaceholders();
	void DrawFormattedCmdLine();

	void DrawCvars();
	void ResetDefaultCvars();
} static g_ImGuiPersist;

// entry point
void ArImGuiTabCallbackEntry()
{
	g_ImGuiPersist.TabCallback();
}

// entry point
void ArImGuiFileDialogWindowEntry()
{
	auto& imfg = *ImGuiFileDialog::Instance();
	if (imfg.Display(ArImGuiPersist::DemoSelector::FILE_DIALOG_ID))
	{
		if (imfg.IsOk())
		{
			std::filesystem::path modDirPath =
			    ArUtf8ToUtf16(ArGlobalPlaceholders::MOD_DIR.GetValue()->c_str());
			auto& mySet = g_ImGuiPersist.demoSelector.paths;
			auto it = mySet.end();
			for (auto& [_, displayPath] : imfg.GetSelection())
			{
				std::filesystem::path asPath = ArUtf8ToUtf16(displayPath.c_str());
				// shorten if possible
				std::error_code ec;
				auto relPath = std::filesystem::relative(displayPath, modDirPath, ec);
				if (!ec && !relPath.empty() && relPath.c_str()[0] != L'.')
					displayPath = relPath.string(); // TODO test with non-ascii
				it = mySet.emplace_hint(it, std::move(asPath), std::move(displayPath));
			}
		}
		imfg.Close();
	}
}

void ArImGuiPersist::TabCallback()
{
	auto multiDemoJobStat = SptAutoRender::GetRunningMultiDemoJobStatus();
	auto runningJobStat = SptAutoRender::GetRunningMovieJobStatus();
	auto lastJobResult = SptAutoRender::GetLastMovieJobResult();

	// job statuses

	ImGui::BeginDisabled(!SptAutoRender::MultiDemoJobWorks());
	DrawMultiDemoJobStatus(multiDemoJobStat.get(), runningJobStat.get());
	ImGui::EndDisabled();

	DrawRunningJobStatus(runningJobStat.get());
	DrawLastFinishedJobStatus(lastJobResult.get());

	// control buttons

	ImGui::BeginDisabled(!!multiDemoJobStat || !!runningJobStat);
	if (ImGui::Button("Start rendering single video"))
		SptAutoRender::QueueSingleMovieJob(CreateDeferredJob());
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(!multiDemoJobStat && !runningJobStat);
	if (ImGui::Button("Stop rendering"))
	{
		SptAutoRender::StopMultiDemoJob();
		SptAutoRender::StopMovieJob();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	DrawLogFolderButton();
	ImGui::BeginDisabled(!!multiDemoJobStat || !!runningJobStat || !SptAutoRender::MultiDemoJobWorks());
	if (ImGui::TreeNode("Render demos"))
	{
		ImGui::BeginDisabled(demoSelector.paths.empty());
		if (ImGui::Button("Start"))
		{
			std::vector<std::filesystem::path> submitPaths;
			std::ranges::transform(demoSelector.paths,
			                       std::back_inserter(submitPaths),
			                       &DemoSelector::demoFilePath::first);
			SptAutoRender::QueueMultiDemoJob(CreateDeferredJob(), std::move(submitPaths));
		}
		ImGui::EndDisabled();
		ImGui::SameLine();

		ImGui::SameLine();
		DrawDemoPaths();
		ImGui::TreePop();
	}
	ImGui::EndDisabled();

	// settings

	DrawFfmpegPath();
	DrawPipeNameOptions();
	DrawFramerateOptions();
	DrawAudioOptions();
	ImGui::Checkbox("Render when console is open", &recordWhenConsoleIsOpen);
	ImGui::Checkbox("Render after ImGui callbacks", &recordAfterImGuiCallbacks);
	ImGui::SameLine();
	SptImGui::HelpMarker("If set, ImGui windows will show up in the recording");

	DrawSyncMode();

	if (DrawUnformattedCmdLine())
		ArGlobalPlaceholders::imGuiFormattedCmdLineDirty.store(true);
	DrawDefaultPlaceholders();

	if (ArGlobalPlaceholders::imGuiFormattedCmdLineDirty.exchange(false))
	{
		cmdLineFormatted = ArPlaceholder::FormatString(ArGlobalPlaceholders::GetAll(),
		                                               cmdLineUnformatted,
		                                               &unrecognizedPlaceholders);
	}

	DrawFormattedCmdLine();

	// cvar settings

	if (userCvarSettings.empty())
		ResetDefaultCvars();
	Assert(std::get<std::string>(userCvarSettings[0].cvar) == "host_framerate");
	Assert(std::get<std::string>(userCvarSettings[1].cvar) == "fps_max");
	float hostFramerateVal = outputFramerate / playbackSpeed;
	userCvarSettings[0].val = std::to_string(hostFramerateVal);
	userCvarSettings[1].val = std::to_string(hostFramerateVal * 0.9f);
	DrawCvars();
}

void ArImGuiPersist::DrawMultiDemoJobStatus(const ArRunningMultiDemoJobStatus* multiDemoJobStat,
                                            const ArRunningMovieJobStatus* curMovieJobStat)
{
	if (!multiDemoJobStat)
		return;

	// TODO needs a title or something
	SptImGui::BeginBordered();

	size_t demoIdx = multiDemoJobStat->nextDemoIdx.load(std::memory_order_acquire);
	if (curMovieJobStat)
		demoIdx--;

	// TODO test with non-ascii file paths
	size_t nDemos = multiDemoJobStat->demoFilePaths.size();
	std::string demoFileName =
	    demoIdx < nDemos ? multiDemoJobStat->demoFilePaths[demoIdx].filename().string() : "ERROR";

	if (curMovieJobStat)
		ImGui::Text("Rendering demo: \"%s\" (%u/%u)", demoFileName.c_str(), demoIdx + 1, nDemos);
	else
		ImGui::Text("Next demo: \"%s\" (%u/%u)", demoFileName.c_str(), demoIdx, nDemos);

	auto elapsedTotal =
	    std::chrono::round<std::chrono::milliseconds>(ar_elapsed_time_clock::now() - multiDemoJobStat->startTime);
	ImGui::Text("Elapsed real time: %s", std::format("{:%T}", elapsedTotal).c_str());

	SptImGui::EndBordered();
}

void ArImGuiPersist::DrawRunningJobStatus(const ArRunningMovieJobStatus* runningStat)
{
	// TODO put in bordered
	if (runningStat)
	{
		const char* statusText;
		if (!runningStat->recordWhenConsoleIsOpen && interfaces::_engine_client->Con_IsVisible())
			statusText = "PAUSED (console is open)";
		else
			statusText = "RUNNING";
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "Status: %s", statusText);

		auto elapsedTotal = std::chrono::round<std::chrono::milliseconds>(ar_elapsed_time_clock::now()
		                                                                  - runningStat->startTime);
		ImGui::Text("Elapsed real time: %s", std::format("{:%T}", elapsedTotal).c_str());

		size_t nConsumedFrames = runningStat->nFramesConsumed.load(std::memory_order_acquire);
		if (nConsumedFrames > 0)
		{
			ImGui::SameLine();
			auto elapsedNoPauses = std::chrono::duration_cast<std::chrono::milliseconds>(
			    runningStat->unpausedElapsedTime.load());
			ImGui::Text("(%.3fms/frame)", (float)elapsedNoPauses.count() / nConsumedFrames);
		}
		ImGui::Text("Consumed %u frames", nConsumedFrames);

		ImGui::SameLine();
		ImGui::Text("(movie of length %.3fs)", nConsumedFrames / runningStat->framerate);
	}
	else
	{
		ImGui::Text("Status: %s", "NOT ACTIVE");
	}
}

void ArImGuiPersist::DrawLastFinishedJobStatus(const ArMovieJobResult* lastResult)
{
	if (!lastResult)
		return;

	SptImGui::BeginBordered();
	if (lastResult->returnCode.has_value())
		ImGui::Text("Last return code: %d", (int)lastResult->returnCode.value());
	if (!lastResult->stat.Ok())
		ImGui::Text("Error: %s", lastResult->stat.GetStatus().errMsg.c_str());
	auto elapsedWithPauses = std::chrono::round<std::chrono::milliseconds>(lastResult->elapsedTime);
	auto elapsedWithoutPauses = std::chrono::round<std::chrono::milliseconds>(lastResult->unpausedElapsedTime);
	ImGui::Text("Ran in: %s, (%s without pauses)",
	            std::format("{:%T}", elapsedWithPauses).c_str(),
	            std::format("{:%T}", elapsedWithoutPauses).c_str());
	ImGui::Text("Consumed %u frames", lastResult->nFramesConsumed);
	if (lastResult->nFramesConsumed > 0)
	{
		ImGui::SameLine();
		ImGui::Text("(%.3fms/frame)", (float)elapsedWithoutPauses.count() / lastResult->nFramesConsumed);
	}
	SptImGui::EndBordered();
}

std::unique_ptr<ArDeferredMovieJob> ArImGuiPersist::CreateDeferredJob()
{
	std::vector<ArCvarSetting> jobCvars = userCvarSettings;
	Assert(std::get<std::string>(userCvarSettings[1].cvar) == "fps_max");
	if (!captureAudio)
		jobCvars.erase(jobCvars.begin() + 1); // remove fps_max if not capturing audio

	// construct on the stack then move into a unique_ptr so we can use designated initializers
	ArDeferredMovieJob defferedMovieJob{
	    .unformattedCmdLine = cmdLineUnformatted,
	    .syncMode = syncMode,
	    .nFramesInFlight = (size_t)nFramesInFlight,
	    .cvars = jobCvars,
	    .volume = volume,
	    .framerate = outputFramerate,
	    .captureAudio = captureAudio,
	    .recordWhenConsoleIsOpen = recordWhenConsoleIsOpen,
	    .recordAfterImGuiCallbacks = recordAfterImGuiCallbacks,
	};

	return std::make_unique<ArDeferredMovieJob>(std::move(defferedMovieJob));
}

void ArImGuiPersist::DrawDemoPaths()
{
	auto modDir = ArGlobalPlaceholders::MOD_DIR.GetValue();

	auto& imfg = *ImGuiFileDialog::Instance();
	ImGui::BeginDisabled(imfg.IsOpened());
	if (ImGui::Button("Add demos"))
	{
		/*
		* If you're reading this, at some point it may have crossed your mind that I can just
		* open a file dialog in windows for the demo selection. Yes I can! But (I think) all of
		* the standard ways of doing that will block this thread, which I find to be less than
		* ideal. Even if we start the dialog in a separate thread, we still need to have some
		* way of force cancelling it in the case that SPT is unloaded while the selection
		* window is open. As far as I can tell, there are no sane ways of doing that! It turns
		* out to be much easier and less botched to use ImGuiFileDialog than windows!
		*/

		// TODO icons
		// TODO see https://github.com/aiekick/ImGuiFileDialog/blob/master/Documentation.md#places- for serialization
		IGFD::FileDialogConfig config{
		    .path = *modDir,
		    .countSelectionMax = 0,
		    .flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DisableCreateDirectoryButton
		             | ImGuiFileDialogFlags_NaturalSorting | ImGuiFileDialogFlags_HideColumnType
		             | ImGuiFileDialogFlags_DisableThumbnailMode | ImGuiFileDialogFlags_ShowDevicesButton,
		};

		imfg.OpenDialog(DemoSelector::FILE_DIALOG_ID, "Select demo files", ".dem", config);
	}
	ImGui::EndDisabled();

	auto& demoSet = demoSelector.paths;
	ImGui::SameLine();
	if (ImGui::Button("Clear demo list"))
		demoSet.clear();

	if (!demoSet.empty())
	{
		ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
		                             | ImGuiTableFlags_NoHostExtendX | ImGuiTableFlags_NoKeepColumnsVisible;
		if (ImGui::BeginTable("demo_list", 2, tableFlags))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.f, 0.f));

			ImGui::TableSetupColumn("index", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("file path", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();

			int idx = 0;
			for (auto it = demoSet.begin(); it != demoSet.end(); idx++)
			{
				ImGui::PushID(idx);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				bool erase = ImGui::SmallButton("-");
				ImGui::SameLine();
				ImGui::Text("%d", idx);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(it->second.c_str(), it->second.c_str() + it->second.size());
				ImGui::PopID();
				if (erase)
					it = demoSet.erase(it);
				else
					++it;
			}

			ImGui::PopStyleVar();
			ImGui::EndTable();
		}
	}
}

bool ArImGuiPersist::DrawLogFolderButton()
{
	if (!ImGui::Button("Open log folder"))
		return false;

	std::wstring wWorkingDirStr = ArUtf8ToUtf16(ArGlobalPlaceholders::RENDER_WORKING_DIR.GetValue()->c_str());
	if (!wWorkingDirStr.empty())
	{
		std::filesystem::path workingDir = wWorkingDirStr;
		std::error_code ec;
		std::filesystem::create_directories(workingDir, ec);
		ShellExecuteW(NULL, L"open", wWorkingDirStr.c_str(), NULL, NULL, SW_SHOWDEFAULT);
	}

	return true;
}

void ArImGuiPersist::DrawFfmpegPath()
{
	if (!lastFfmpegSearchSuccess.has_value())
		lastFfmpegSearchSuccess = ArGlobalPlaceholders::EXE_PATH.FindFfmpeg();

	{
		static std::string tmp;
		tmp = *ArGlobalPlaceholders::EXE_PATH.GetValue();
		if (ImGui::InputText("Exe path", &tmp, ImGuiInputTextFlags_ElideLeft))
			ArGlobalPlaceholders::EXE_PATH.SetValue(tmp);
	}

	ImGui::SameLine();
	if (ImGui::Button("Auto-detect"))
		lastFfmpegSearchSuccess = ArGlobalPlaceholders::EXE_PATH.FindFfmpeg();
	ImGui::SetItemTooltip("search for ffmpeg in the PATH");
	ImGui::SameLine();
	SptImGui::HelpMarker(
	    "This is the application that all data will be pumped into.\n"
	    "By default it's ffmpeg, but you can in theory pump it into\n"
	    "a custom application that can process the .nut format.");

	if (!lastFfmpegSearchSuccess.value())
	{
		ImGui::TextColored(
		    SPT_IMGUI_WARN_COLOR_YELLOW,
		    "Warning: ffmpeg.exe not found in PATH. Type in the path manually or add it to your PATH.");
		ImGui::SameLine();
		ImGui::TextLinkOpenURL("Download link", "https://www.gyan.dev/ffmpeg/builds/ffmpeg-git-full.7z");
	}
}

void ArImGuiPersist::DrawPipeNameOptions()
{
	const char* pipeNameOpts[] = {"Keep name consistent", "Append UUID (default)"};
	if (ImGui::Combo("Pipe name",
	                 &ArGlobalPlaceholders::PIPE_NAME.appendUuid,
	                 pipeNameOpts,
	                 ARRAYSIZE(pipeNameOpts)))
	{
		ArGlobalPlaceholders::PIPE_NAME.Regenerate();
	}
	ImGui::SameLine();
	SptImGui::HelpMarker(
	    "Advanced users only!\n"
	    "By default, a random name is used for the video/audio pipes for every video.\n"
	    "This allows rendering with multiple instances and minimizes bugs if this code fails to close ffmpeg correctly.\n"
	    "This option is mostly for debugging.");
}

void ArImGuiPersist::DrawFramerateOptions()
{
	if (ImGui::InputFloat("Output framerate", &outputFramerate))
	{
		outputFramerate = std::clamp(outputFramerate, 1.f, 500.f);
		ArGlobalPlaceholders::FRAMERATE.SetValue(std::to_string(outputFramerate));
	}

	// TODO test with different playback speed settings
	/*if (ImGui::InputFloat("Playback speed", &playbackSpeed))
		playbackSpeed = std::clamp(playbackSpeed, 0.001f, 1000.f);*/
}

void ArImGuiPersist::DrawAudioOptions()
{
	bool hasSupport = SptAutoRender::SupportsAudioCapture();
	captureAudio &= hasSupport;
	ImGui::BeginDisabled(!hasSupport);
	ImGui::Checkbox("Capture audio", &captureAudio);
	ImGui::BeginDisabled(!captureAudio);
	ImGui::SliderFloat("Volume", &volume, 0.f, 1.f, nullptr, ImGuiSliderFlags_AlwaysClamp);
	ImGui::SameLine();
	SptImGui::HelpMarker("This behaves pretty much the same as the normal game volume, but is independent of it.");
	ImGui::EndDisabled();
	ImGui::EndDisabled();
}

void ArImGuiPersist::DrawSyncMode()
{
	ImGui::TextUnformatted("Sync mode:");
	ImGui::SameLine();
	ImGui::RadioButton("Synchronous", (int*)&syncMode, AR_SYNC_FULL);
	ImGui::SameLine();
	ImGui::RadioButton("Threaded", (int*)&syncMode, AR_SYNC_THREADED);

	ImGui::SameLine();
	SptImGui::HelpMarker(
	    "Determines how frames are submitted to the app:\n"
	    " - Synchronous: frames are pumped in on the render thread\n"
	    " - Threaded: frames are asynchronously requested from the GPU and pumped in by a worker thread\n"
	    "\n"
	    "The threaded option should be good and fast, the synchronous option exists as a slower fallback.");

	ImGui::BeginDisabled(syncMode == AR_SYNC_FULL);
	ImGui::SliderInt("Number of frames in flight", &nFramesInFlight, 1, 3, nullptr, ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
}

bool ArImGuiPersist::DrawUnformattedCmdLine()
{
	bool changed = false;

	if (resetCmdLine)
	{
		cmdLineUnformatted = ArCreateDefaultCmdLine();
		resetCmdLine = false;
		changed = true;
	}

	ImGui::TextUnformatted("Program to execute:");
	if (ImGui::InputTextMultiline("##cmdline_unformatted",
	                              &cmdLineUnformatted,
	                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 16),
	                              ImGuiInputTextFlags_WordWrap))
	{
		changed = true;
	}

	if (ImGui::Button("Reset to default"))
	{
		cmdLineUnformatted = ArCreateDefaultCmdLine();
		changed = true;
	}

	return changed;
}

void ArImGuiPersist::DrawDefaultPlaceholders()
{
	// TODO this doesn't work!!!
	if (!unrecognizedPlaceholders.empty())
	{
		std::string out = "Warning: unrecognized placeholder(s): ";
		for (auto& s : unrecognizedPlaceholders)
		{
			out += s;
			out += ", ";
		}
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "%.*s", out.size() - 2, out.c_str());
	}

	if (ImGui::TreeNode("Available placeholders"))
	{
		if (ImGui::BeginTable("##placeholders",
		                      3,
		                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX
		                          | ImGuiTableFlags_BordersInner | ImGuiTableFlags_BordersOuter))
		{
			ImGui::TableSetupColumn("Key");
			ImGui::TableSetupColumn("Value");
			ImGui::TableSetupColumn("Description");
			ImGui::TableHeadersRow();

			for (const ArPlaceholder* pl : ArGlobalPlaceholders::GetAll())
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(pl->key.c_str());

				ImGui::TableNextColumn();
				auto val = pl->GetValue();
				ImGui::TextUnformatted(val ? val->c_str() : "-");

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(pl->helpText.c_str());
			}
			ImGui::EndTable();
		}

		ImGui::TreePop();
	}
}

void ArImGuiPersist::DrawFormattedCmdLine()
{
	if (ImGui::TreeNode("Formatted command"))
	{
		ImGui::InputTextMultiline("##cmdline_formated",
		                          &cmdLineFormatted,
		                          ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 16),
		                          ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
		ImGui::TreePop();
	}
}

void ArImGuiPersist::DrawCvars()
{
	if (ImGui::TreeNode("ConVars"))
	{
		ImGui::TextWrapped(
		    "These are cvars that will be set when the rendering is started and set back to their prior values afterwards.");
		if (ImGui::Button("Reset to default"))
			ResetDefaultCvars();

		if (ImGui::BeginTable("##cvars", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("cvar name");
			ImGui::TableSetupColumn("value");
			ImGui::TableHeadersRow();

			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));

			int deleteIdx = -1;
			for (size_t i = 0; i < userCvarSettings.size(); i++)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushID((int)i);
				bool isDisabled =
				    i < defaultCvarSettings.size() && defaultCvarSettings[i].helpText.has_value();
				ImGui::BeginDisabled(isDisabled);
				ImGui::InputText("##name", &std::get<std::string>(userCvarSettings[i].cvar));
				if (isDisabled)
				{
					ImGui::SameLine();
					SptImGui::HelpMarker(defaultCvarSettings[i].helpText.value().c_str());
				}
				else
				{
					ImGui::SameLine();
					if (ImGui::SmallButton("-"))
						deleteIdx = (int)i;
				}
				ImGui::TableNextColumn();
				ImGui::InputText("##value", &userCvarSettings[i].val);
				ImGui::EndDisabled();
				ImGui::PopID();
			}

			ImGui::PopStyleVar();

			if (deleteIdx >= 0)
				userCvarSettings.erase(userCvarSettings.begin() + deleteIdx);

			ImGui::EndTable();
		}

		if (ImGui::SmallButton("+"))
			userCvarSettings.push_back(ArCvarSetting{"", ""});

		ImGui::TreePop();
	}
}

void ArImGuiPersist::ResetDefaultCvars()
{
	userCvarSettings.clear();
	std::transform(defaultCvarSettings.begin(),
	               defaultCvarSettings.end(),
	               std::back_inserter(userCvarSettings),
	               [](const DefaultCvar& def) { return def.cvarSetting; });
}
