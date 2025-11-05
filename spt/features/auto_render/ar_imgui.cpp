#include "stdafx.hpp"

#include "ar_interface.hpp"
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

	struct DefaultCvar
	{
		ArCvarSetting cvarSetting;
		std::optional<std::string> helpText = std::nullopt; // if set, this is a mutable cvar
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
	    DefaultCvar{{"spt_disable_tone_map_reset", "1"}},
	    DefaultCvar{{"spt_override_tpose", "17"}},
	};

	std::vector<ArCvarSetting> userCvarSettings;

	float fpsVal = 60.f;
	float playbackSpeed = 1.f;
	float volume = .5f;
	bool captureAudio = true;
	bool recordWhenConsoleIsOpen = false;
	bool recordAfterImGuiCallbacks = false;

	void TabCallback();

	bool DrawRunningJobStatus();      // returns true if there's an active job
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

	void DrawCvars();
	void ResetDefaultCvars();
};

// entry point
void ArImGuiTabCallback()
{
	static ArImGuiPersist persist;
	persist.TabCallback();
}

void ArImGuiPersist::TabCallback()
{
	// job statuses

	bool curJobRunning = DrawRunningJobStatus();
	[[maybe_unused]] bool lastJobExists = DrawLastFinishedJobStatus();

	// control buttons

	ImGui::BeginDisabled(curJobRunning);
	DrawStartRenderButton();
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(!curJobRunning);
	if (ImGui::Button("Stop rendering"))
		SptAutoRender::StopMovieJob();
	ImGui::EndDisabled();

	ImGui::SameLine();

	DrawLogFolderButton();

	// settings

	DrawFfmpegPath();
	DrawPipeNameOptions();
	DrawFramerateOptions();
	DrawAudioOptions();
	ImGui::Checkbox("Render when console is open", &recordWhenConsoleIsOpen);
	ImGui::Checkbox("Render after ImGui callbacks", &recordAfterImGuiCallbacks);
	ImGui::SameLine();
	SptImGui::HelpMarker("If set, ImGui windows will show up in the recording");

	// TODO - option for different videohook
	// TODO - option for rendering demos or number of frames

	DrawSyncMode();

	// cmdline TODO: wrap

	if (DrawUnformattedCmdLine())
		ArGlobalPlaceholders::imGuiFormattedCmdLineDirty.store(true);
	DrawDefaultPlaceholders();

	if (ArGlobalPlaceholders::imGuiFormattedCmdLineDirty.exchange(false))
	{
		// TODO - reformat the string with per-video placeholders if the user presses the start button
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
	float hostFramerateVal = fpsVal / playbackSpeed;
	userCvarSettings[0].val = std::to_string(hostFramerateVal);
	userCvarSettings[1].val = std::to_string(hostFramerateVal * 0.9f);
	DrawCvars();
}

bool ArImGuiPersist::DrawRunningJobStatus()
{
	// TODO: report number of demos, output file name, ms/frame

	auto runningStat = SptAutoRender::GetRunningMovieJobStatus();

	if (runningStat)
	{
		const char* statusText;
		if (runningStat->userPaused)
			statusText = "PAUSED (user request)";
		else if (!runningStat->recordWhenConsoleIsOpen && interfaces::_engine_client->Con_IsVisible())
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
		ImGui::Text("(movie of length %.3fs)", nConsumedFrames / runningStat->outputFramerate);
	}
	else
	{
		ImGui::Text("Status: %s", "NOT ACTIVE");
	}

	return !!runningStat;
}

bool ArImGuiPersist::DrawLastFinishedJobStatus()
{
	auto lastResult = SptAutoRender::GetLastMovieJobResult();
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

	std::vector<ArCvarSetting> jobCvars = userCvarSettings;
	Assert(std::get<std::string>(userCvarSettings[1].cvar) == "fps_max");
	if (!captureAudio)
		jobCvars.erase(jobCvars.begin() + 1); // remove fps_max if not capturing audio

	// construct on the stack then move into a unique_ptr so we can use designated initializers
	ArDeferredMovieJob defferedMovieJob{
	    .unformattedCmdLine = cmdLineUnformatted,
	    .syncMode = syncMode,
	    .nFramesInFlight = (size_t)nFramesInFlight,
	    .controller = nullptr, // TODO
	    .cvars = jobCvars,
	    .volume = volume,
	    .recordWhenConsoleIsOpen = recordWhenConsoleIsOpen,
	    .recordAfterImGuiCallbacks = recordAfterImGuiCallbacks,
	};

	SptAutoRender::QueueMovieJob(std::make_unique<ArDeferredMovieJob>(std::move(defferedMovieJob)));

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
	if (ImGui::InputFloat("Output framerate", &fpsVal))
	{
		fpsVal = std::clamp(fpsVal, 1.f, 500.f);
		ArGlobalPlaceholders::FRAMERATE.SetValue(std::to_string(fpsVal));
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
