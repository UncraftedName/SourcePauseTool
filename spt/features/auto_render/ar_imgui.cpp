#include "stdafx.hpp"

#include "ar_feature.hpp"
#include "ar_util.hpp"
#include "spt/features/visualizations/imgui/imgui_interface.hpp"
#include "spt/utils/interfaces.hpp"
#include "thirdparty/imgui/imgui_stdlib.h"

#undef clamp

// TODO - move to public API so we can call this from a concmd
static std::string ArCreateDefaultCmdLine()
{
	return std::format(
	    "\"{0}\" -report -nostdin "
	    "-f nut -i \"{1}\" "
	    "-y -c:v libx264 -pix_fmt yuv420p -crf 18 "
	    "-c:a aac -b:a 192k "
	    "-shortest -preset veryfast \"{2}\\{3}.mp4\"",
	    ArGlobalPlaceholders::EXE_PATH.key,
	    ArGlobalPlaceholders::PIPE_NAME.key,
	    ArGlobalPlaceholders::RENDER_WORKING_DIR.key,
	    ArGlobalPlaceholders::DATE_TIME.key);
}

// TODO figure out where to put these guys - we want access to them from a cmd as well
// TODO if rendering a folder of demos into multiple videos, the default output filename should be changed

/*AR_PER_VID_PLACEHOLDER(MAP_NAME, "The name of the loaded map on the first frame of recording");
AR_PER_VID_PLACEHOLDER(MAP_SEQ, "The map index starting at 0 on the first frame of recording");
AR_PER_VID_PLACEHOLDER(DEMO_SEQ, "The demo index starting at 0 (only applicable if rendering demos)");
AR_PER_VID_PLACEHOLDER(DEMO_FILENAME, "The demo file name (only applicable if rendering demos)");*/

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

	float fpsVal = std::stoi(*ArGlobalPlaceholders::FRAMERATE.GetValue());
	float playbackSpeed = 1.f;
	float volume = .5f;
	bool captureAudio = true;
	bool recordWhenConsoleIsOpen = false;
	bool recordAfterImGuiCallbacks = false;

	bool DrawCurJobStatus();          // returns true if there's an active job
	bool DrawLastFinishedJobStatus(); // returns true if there's a last job

	bool DrawStartRenderButton(); // returns true if rendering was started
	bool DrawLogFolderButton();   // returns true if log folder was opened

	void DrawFfmpegPath();
	void DrawPipeNameOptions();
	void DrawFramerateOptions();
	void DrawAudioOptions();
	void DrawSyncMode();

	bool DrawUnformattedCmdLine(); // returns true if was updated
	void DrawDefaultPlaceholders();
	void DrawFormattedCmdLine();
};

void AutoRenderFeature::ImGuiTabCallback()
{
	static ArImGuiPersist persist;

	imGuiCallbackActive.store(true); // let render thread know that it can update stuff for imgui

	// job statuses

	bool curJobRunning = persist.DrawCurJobStatus();
	[[maybe_unused]] bool lastJobExists = persist.DrawLastFinishedJobStatus();

	// control buttons

	ImGui::BeginDisabled(curJobRunning);
	persist.DrawStartRenderButton();
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(!curJobRunning);
	if (ImGui::Button("Stop rendering"))
		spt_auto_render_feat.StopMovieJob();
	ImGui::EndDisabled();

	ImGui::SameLine();

	persist.DrawLogFolderButton();

	// settings

	persist.DrawFfmpegPath();
	persist.DrawPipeNameOptions();
	persist.DrawFramerateOptions();
	persist.DrawAudioOptions();
	ImGui::Checkbox("Render when console is open", &persist.recordWhenConsoleIsOpen);
	ImGui::Checkbox("Render after ImGui callbacks", &persist.recordAfterImGuiCallbacks);
	ImGui::SameLine();
	SptImGui::HelpMarker("If set, ImGui windows will show up in the recording");

	// TODO - option for different videohook
	// TODO - option for rendering demos or number of frames

	persist.DrawSyncMode();

	// cmdline TODO: wrap

	if (persist.DrawUnformattedCmdLine())
		ArGlobalPlaceholders::imGuiFormattedCmdLineDirty.store(true);
	persist.DrawDefaultPlaceholders();

	if (ArGlobalPlaceholders::imGuiFormattedCmdLineDirty.exchange(false))
	{
		// TODO - reformat the string with per-video placeholders if the user presses the start button
		persist.cmdLineFormatted = ArPlaceholder::FormatString(ArGlobalPlaceholders::GetAll(),
		                                                       persist.cmdLineUnformatted,
		                                                       &persist.unrecognizedPlaceholders);
	}

	persist.DrawFormattedCmdLine();
}

bool ArImGuiPersist::DrawCurJobStatus()
{
	// TODO: report number of demos, output file name, ms/frame

	auto runningJob = spt_auto_render_feat.GetRunningJobStatus();

	if (runningJob)
	{
		const char* statusText;
		if (runningJob->userPaused)
			statusText = "PAUSED (user request)";
		else if (!runningJob->recordWhenConsoleIsOpen && interfaces::_engine_client->Con_IsVisible())
			statusText = "PAUSED (console is open)";
		else
			statusText = "RUNNING";
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "Status: %s", statusText);
		auto elapsedTotal =
		    std::chrono::round<std::chrono::milliseconds>(ar_elapsed_time_clock::now() - runningJob->startTime);
		ImGui::Text("Elapsed time: %s", std::format("{:%T}", elapsedTotal).c_str());
		// TODO - this should be a reference wrapper to the result which has the value or something
		ar_frame_idx nConsumedFrames = runningJob->nFramesConsumed;
		if (nConsumedFrames > 0)
		{
			ImGui::SameLine();
			auto elapsedNoPauses = std::chrono::duration_cast<std::chrono::milliseconds>(
			    runningJob->unpausedElapsedTime.load());
			ImGui::Text("(%.3fms/frame)", elapsedNoPauses.count() / nConsumedFrames);
		}

		std::optional<ar_frame_idx> nMaxFrames = runningJob->maxConsumeFrames;
		if (nMaxFrames)
			ImGui::Text("Consumed %u/%u frames", nConsumedFrames, nMaxFrames.value());
		else
			ImGui::Text("Consumed %u frames", nConsumedFrames);
	}
	else
	{
		ImGui::Text("Status: %s", "NOT ACTIVE");
	}

	return !!runningJob;
}

bool ArImGuiPersist::DrawLastFinishedJobStatus()
{
	auto lastResult = spt_auto_render_feat.GetLastMovieJobResult();
	if (!lastResult)
		return false;

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

	return true;
}

bool ArImGuiPersist::DrawStartRenderButton()
{
	if (!ImGui::Button("Start rendering"))
		return false;

	// construct on the stack then move into a unique_ptr so we can use designated initializers
	ArDeferredMovieJob defferedMovieJob{
	    .ffmpegArgs{
	        .ffmpegWorkingDir = ArUtf8ToUtf16(ArGlobalPlaceholders::RENDER_WORKING_DIR.GetValue()->c_str()),
	        .cmd = ArUtf8ToUtf16(cmdLineFormatted.c_str()),
	        .pipeName = ArUtf8ToUtf16(ArGlobalPlaceholders::PIPE_NAME.GetValue()->c_str()),
	        .width = (size_t)atoi(ArGlobalPlaceholders::VID_WIDTH.GetValue()->c_str()),
	        .height = (size_t)atoi(ArGlobalPlaceholders::VID_HEIGHT.GetValue()->c_str()),
	        .framerate = (float)atof(ArGlobalPlaceholders::FRAMERATE.GetValue()->c_str()),
	        .captureAudio = captureAudio,
	    },
	    .maxConsumeFrames = std::nullopt, // TODO
	    .syncMode = syncMode,
	    .nFramesInFlight = (size_t)nFramesInFlight,
	    .cvars = spt_auto_render_feat.CreateDefaultCvarSettings(fpsVal / playbackSpeed), // TODO
	    .volume = volume,
	    .recordWhenConsoleIsOpen = recordWhenConsoleIsOpen,
	    .recordAfterImGuiCallbacks = recordAfterImGuiCallbacks,
	};

	spt_auto_render_feat.QueueMovieJob(std::make_unique<ArDeferredMovieJob>(std::move(defferedMovieJob)));

	return true;
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
		if (ImGui::InputText("Exe path", &tmp))
			ArGlobalPlaceholders::EXE_PATH.SetValue(tmp);
	}

	ImGui::SameLine();
	if (ImGui::Button("Auto-detect"))
		lastFfmpegSearchSuccess = ArGlobalPlaceholders::EXE_PATH.FindFfmpeg();
	ImGui::SetItemTooltip("search for ffmpeg in the PATH");
	ImGui::SameLine();
	SptImGui::HelpMarker(
	    "This is the application that all data will be pumped into.\n"
	    "By default it's ffmpeg, but you can in theory use any application\n"
	    "that accepts named pipes as input for raw video and audio streams.");

	if (!lastFfmpegSearchSuccess.value())
	{
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "Warning: ffmpeg.exe not found,");
		ImGui::SameLine();
		ImGui::TextLinkOpenURL("download it", "https://www.gyan.dev/ffmpeg/builds/ffmpeg-git-full.7z");
		ImGui::SameLine();
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "and add it to your PATH.");
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
	if (ImGui::InputFloat("Framerate", &fpsVal))
	{
		fpsVal = std::clamp(fpsVal, 1.f, 500.f);
		ArGlobalPlaceholders::FRAMERATE.SetValue(std::to_string(fpsVal));
	}

	if (ImGui::InputFloat("Playback speed", &playbackSpeed))
		playbackSpeed = std::clamp(playbackSpeed, 0.001f, 1000.f);
}

void ArImGuiPersist::DrawAudioOptions()
{
	bool hasSupport = spt_auto_render_feat.SupportsAudioCapture();
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

	ImGui::BeginDisabled(syncMode == AR_SYNC_FULL);
	ImGui::SliderInt("Number of frames in flight", &nFramesInFlight, 1, 3, nullptr, ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();

	ImGui::SameLine();
	SptImGui::HelpMarker(
	    "Determines how frames are submitted to the app:\n"
	    " - Synchronous: frames are pumped in on the render thread\n"
	    " - Threaded: frames are asynchronously requested from the GPU and pumped in by a worker thread\n"
	    "\n"
	    "The threaded option should be good and fast, the synchronous option exists as a slower fallback.");
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
	                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 16)))
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
		                          ImGuiInputTextFlags_ReadOnly);
		ImGui::TreePop();
	}
}
