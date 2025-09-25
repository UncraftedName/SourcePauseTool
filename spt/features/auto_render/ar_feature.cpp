#include "stdafx.hpp"

#include "ar_decls.hpp"
#include "ar_util.hpp"
#include "spt/feature.hpp"
#include "spt/features/visualizations/imgui/imgui_interface.hpp"
#include "spt/utils/interfaces.hpp"

#include "thirdparty/imgui/imgui_stdlib.h"

#include <variant>
#include <chrono>
#include <mutex>

#include <Rpc.h>
#pragma comment(lib, "Rpcrt4")

enum ArSyncMode : int
{
	AR_SYNC_FULL,
	AR_SYNC_ASYNC,
	AR_SYNC_THREADED,
};

// TODO this is kinda cringe
struct ArAppResult
{
	ar_frame_idx nFramesConsumed;
	std::optional<DWORD> returnCode;
	std::chrono::nanoseconds duration;
	ser::StatusTracker stat;
};

struct ArDeferredMovieJob
{
	ArFfmpegWriter::InitArgs args;
	std::unique_ptr<ArAppResult> result;
	std::optional<ar_frame_idx> maxNFrames;
	ArSyncMode syncMode;
	size_t nFramesInFlight; // only used if asyncMode != AR_SYNC_FULL
};

struct ArRunningJob
{
	using clock = std::chrono::high_resolution_clock;

	clock::time_point startTime;
	std::unique_ptr<ArAppResult> result;
	std::optional<ar_frame_idx> maxConsumeFrames;
	std::unique_ptr<ArSyncManager> mgr;
	// ImGui may request a kill -> kill actually happens in present hook
	bool killSignal = false;

	auto GetElapsedTime() const
	{
		return clock::now() - startTime;
	}
};

namespace patterns
{
	PATTERNS(S_TransferStereo16,
	         "portal1-5135",
	         "53 55 56 57 E8 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ??");
}

struct portable_samplepair_t
{
	int left;
	int right;
};

// placeholders

class AutoRenderFeature : public FeatureWrapper<AutoRenderFeature>
{
public:
	static inline std::atomic<bool> imGuiCallbackActive = false;
	bool appendUuidToPipes = true;

	static inline std::mutex jobMtx;
	std::unique_ptr<ArDeferredMovieJob> deferredMovieJob;
	std::unique_ptr<ArRunningJob> runningMovieJob;
	std::unique_ptr<ArAppResult> lastAppResult;

protected:
	virtual void InitHooks() override;
	virtual void LoadFeature() override;
	virtual void UnloadFeature() override;

private:
	void OnShaderDevicePresentSignal(IDirect3DDevice9* device);
	void ImGuiTabCallback();

	DECL_STATIC_HOOK_CDECL(void,
	                       S_TransferStereo16,
	                       void* pOutput,
	                       const portable_samplepair_t* pfront,
	                       int lpaintedtime,
	                       int endtime);

} static spt_auto_render_feat;

class ArPlaceholder
{
	inline static std::vector<std::reference_wrapper<const ArPlaceholder>> allPlaceHolders;

	explicit ArPlaceholder(const char* key, const char* helpText) : key(key), helpText(helpText)
	{
		allPlaceHolders.push_back(std::ref(*this));
	}

	ArPlaceholder(ArPlaceholder&) = delete;
	ArPlaceholder(ArPlaceholder&&) = delete;

	friend class ArPlaceholders;

public:
	static const auto& GetAll()
	{
		return allPlaceHolders;
	}

	inline static std::atomic<bool> cmdLineFormattedDirty = true;

	const char* key;
	const char* helpText;
	std::atomic<std::shared_ptr<std::string>> val;

	static std::string UnformattedStr(const std::string& s)
	{
		return std::format("{}{}{}", '{', s, '}');
	}

	std::string UnformattedKey() const
	{
		return UnformattedStr(key);
	}

	// if changed, sets the dirty flag for the formatted string
	void SetValue(std::string newVal)
	{
		auto newValPtr = std::make_shared<std::string>(std::move(newVal));
		auto oldVal = val.exchange(newValPtr);
		if (!oldVal || *oldVal != *newValPtr)
			cmdLineFormattedDirty.store(true);
	}

	// return value is const - you must set the value through SetVal to update the dirty flag
	std::shared_ptr<const std::string> GetValue() const
	{
		auto ret = val.load();
		if (ret)
			return ret;
		return ret ? ret : std::make_shared<std::string>("NONE");
	}
};

// default placeholder values are set in LoadFeature
class ArPlaceholders
{
public:
#define AR_DEFINE_PLACEHOLDER(name, helpText) \
	inline static ArPlaceholder name \
	{ \
		#name, helpText \
	}

	AR_DEFINE_PLACEHOLDER(EXE_PATH, "Path to the ffmpeg executable (or an executable of your choice)");
	AR_DEFINE_PLACEHOLDER(VID_WIDTH,
	                      "Input video width in pixels (set to whatever the game is currently running at)");
	AR_DEFINE_PLACEHOLDER(VID_HEIGHT,
	                      "Input video height in pixels (set to whatever the game is currently running at)");
	AR_DEFINE_PLACEHOLDER(FRAMERATE, "Input framerate");
	AR_DEFINE_PLACEHOLDER(VIDEO_PIPE_NAME, "Name of the video pipe that the raw frames will be fed to");
	AR_DEFINE_PLACEHOLDER(AUDIO_PIPE_NAME, "Name of the audio pipe that the stereo PCM will be fed to");
	AR_DEFINE_PLACEHOLDER(RENDER_WORKING_DIR, "Working directory of the rendering application");
	AR_DEFINE_PLACEHOLDER(GAME_WORKING_DIR, "Working directory of the game");
	AR_DEFINE_PLACEHOLDER(MOD_DIR, "Mod directory");
	AR_DEFINE_PLACEHOLDER(UUID, "Unique UUID for each video");
	// TODO these options require special handling and need to be conditionally enabled
	/*AR_DEFINE_PLACEHOLDER(MAP_NAME, "The name of the loaded map on the first frame of recording");
	AR_DEFINE_PLACEHOLDER(MAP_SEQ, "The map index start at 0 on the first frame of recording");
	AR_DEFINE_PLACEHOLDER(DEMO_SEQ, "The demo index starting at 0 (only applicable if rendering demos)");
	AR_DEFINE_PLACEHOLDER(DEMO_FILENAME, "The demo file name (only applicable if rendering demos)");*/

#undef AR_DEFINE_PLACEHOLDER

	static bool FindFFmpeg();
	static void RegenerateUuid();
	static void SetPipeNames();
	static std::string CreateDefaultCmdLine();
	static void FormatCmdLine(const std::string& unformatted,
	                          std::string& formatted,
	                          std::vector<std::string>* unrecognizedPlaceholders);
};

void AutoRenderFeature::InitHooks()
{
	HOOK_FUNCTION(engine, S_TransferStereo16);
}

void AutoRenderFeature::LoadFeature()
{
	if (!ShaderDevicePresentSignal.Works || !SptImGui::Loaded())
		return;

	// fill in default placeholders

	// TODO test with portal in a non-ascii folder
	std::filesystem::path modDir = interfaces::_engine_client->GetGameDirectory();
	modDir = std::filesystem::canonical(modDir);
	ArPlaceholders::MOD_DIR.SetValue(modDir.string());
	std::error_code ec;
	std::filesystem::path workingDir = std::filesystem::current_path(ec);
	ArPlaceholders::GAME_WORKING_DIR.SetValue((ec ? modDir : workingDir).string());
	ArPlaceholders::RENDER_WORKING_DIR.SetValue((workingDir / "spt_autorender").string());
	ArPlaceholders::EXE_PATH.SetValue("C:\\YOUR_MOM\\ffmpeg.exe");
	ArPlaceholders::FRAMERATE.SetValue("60");

	ArPlaceholders::RegenerateUuid();
	ArPlaceholders::SetPipeNames();

	ShaderDevicePresentSignal.Connect(this, &AutoRenderFeature::OnShaderDevicePresentSignal);
	/*InitCommand(spt_ar_screenshot);
	InitCommand(spt_ar_profile);
	InitCommand(spt_ar_stop_all);
	InitCommand(spt_ar_startmovie);
	InitCommand(spt_ar_startmovie_ffmpeg);*/

	SptImGuiGroup::QoL_AutoRender.RegisterUserCallback([this]() { ImGuiTabCallback(); });
}

void AutoRenderFeature::UnloadFeature()
{
	std::lock_guard lk(jobMtx);
	runningMovieJob.reset();
}

void AutoRenderFeature::OnShaderDevicePresentSignal(IDirect3DDevice9* device)
{
	// technically I don't think we should be calling Msg/Warning from the render thread :/

	if (imGuiCallbackActive.exchange(false))
	{
		// update back buffer placeholder
		ser::StatusTracker stat;
		auto [_, desc] = ArGetBackBufferInfo(device, stat);
		if (!stat.Ok())
		{
			Warning("[%s]: %s\n", __FUNCTION__, stat.GetStatus().errMsg.c_str());
			return;
		}
		ArPlaceholders::VID_WIDTH.SetValue(std::to_string(desc.Width));
		ArPlaceholders::VID_HEIGHT.SetValue(std::to_string(desc.Height));
	}

	std::lock_guard lock(jobMtx);

	if (deferredMovieJob)
	{
		if (runningMovieJob)
		{
			runningMovieJob->killSignal = true;
		}
		else
		{
			auto startTime = ArRunningJob::clock::now();
			auto& stat = deferredMovieJob->result->stat;
			std::unique_ptr<ArLockableSurfaceConsumer> consumer =
			    std::make_unique<ArFfmpegWriter>(deferredMovieJob->args, stat);

			std::unique_ptr<ArSyncManager> mgr;
			if (deferredMovieJob->result->stat.Ok())
			{
				switch (deferredMovieJob->syncMode)
				{
				case AR_SYNC_FULL:
					mgr = std::make_unique<ArSynchronousConsumerManager>(device,
					                                                     D3DFMT_A8R8G8B8,
					                                                     std::move(consumer),
					                                                     stat);
					break;
				case AR_SYNC_ASYNC:
					mgr =
					    std::make_unique<ArAsyncConsumerManager>(device,
					                                             D3DFMT_A8R8G8B8,
					                                             deferredMovieJob->nFramesInFlight,
					                                             std::move(consumer),
					                                             stat);
					break;
				case AR_SYNC_THREADED:
					mgr =
					    std::make_unique<ArAsyncConsumerManager>(device,
					                                             D3DFMT_A8R8G8B8,
					                                             deferredMovieJob->nFramesInFlight,
					                                             std::move(consumer),
					                                             stat);
					break;
				default:
					Assert(0);
				}
			}

			runningMovieJob = std::unique_ptr<ArRunningJob>(new ArRunningJob{
			    .startTime = startTime,
			    .result = std::move(deferredMovieJob->result),
			    .maxConsumeFrames = deferredMovieJob->maxNFrames,
			    .mgr = std::move(mgr),
			});
			deferredMovieJob.reset();
		}
	}

	if (runningMovieJob)
	{
		bool shouldKill = runningMovieJob->killSignal || !runningMovieJob->result->stat.Ok();
		if (!shouldKill && runningMovieJob->maxConsumeFrames.has_value())
			shouldKill =
			    runningMovieJob->mgr->GetNumConsumedFrames() >= runningMovieJob->maxConsumeFrames.value();
		if (!shouldKill)
		{
			runningMovieJob->mgr->OnDevicePresent(device, runningMovieJob->result->stat);
			shouldKill = !runningMovieJob->result->stat.Ok();
		}
		if (shouldKill)
		{
			ser::StatusTracker finishStat;
			runningMovieJob->mgr->Finish(device, finishStat);
			if (!finishStat.Ok())
				runningMovieJob->result->stat.Err(finishStat.GetStatus().errMsg);

			lastAppResult = std::move(runningMovieJob->result);
			lastAppResult->nFramesConsumed = runningMovieJob->mgr->GetNumConsumedFrames();
			lastAppResult->duration = runningMovieJob->GetElapsedTime();
			runningMovieJob.reset();
		}
	}
}

void AutoRenderFeature::ImGuiTabCallback()
{
	// gotta keep most static settings up here since the button to start rendering is at the very top :p
	struct
	{
		std::optional<bool> lastFfmpegSearchSuccess;
		bool resetCmdLine = true;
		ArSyncMode syncMode = AR_SYNC_THREADED;
		int nFramesInFlight = 3;

		std::string cmdLineUnformatted;
		std::string cmdLineFormatted;
		std::vector<std::string> unrecognizedPlaceholders;

		float fpsVal = std::stoi(*ArPlaceholders::FRAMERATE.GetValue());

	} static persist;

	imGuiCallbackActive = true; // let render thread know that it can update stuff for imgui

	// start/stop controls

	bool jobRunning;

	{
		// TODO: report number of demos, output file name, ms/frame

		std::lock_guard lk(jobMtx);
		jobRunning = !!runningMovieJob;

		if (jobRunning)
		{
			ar_frame_idx nConsumedFrames = runningMovieJob->mgr->GetNumConsumedFrames();
			std::optional<ar_frame_idx> nMaxFrames = runningMovieJob->maxConsumeFrames;
			ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "Status: RUNNING"); // TODO add paused state
			auto elapsedMs =
			    std::chrono::round<std::chrono::milliseconds>(runningMovieJob->GetElapsedTime());
			ImGui::Text("Elapsed time: %s", std::format("{:%T}", elapsedMs).c_str());
			if (nMaxFrames)
				ImGui::Text("Consumed %u/%u frames", nConsumedFrames, nMaxFrames.value());
			else
				ImGui::Text("Consumed %u frames", nConsumedFrames);
		}
		else
		{
			ImGui::Text("Status: %s", lastAppResult ? "DONE" : "NOT STARTED");
		}

		if (lastAppResult)
		{
			SptImGui::BeginBordered();
			if (lastAppResult->returnCode.has_value())
				ImGui::Text("Last return code: %d", (int)lastAppResult->returnCode.value());
			if (!lastAppResult->stat.Ok())
				ImGui::Text("Error: %s", lastAppResult->stat.GetStatus().errMsg.c_str());
			auto elapsedMs = std::chrono::round<std::chrono::milliseconds>(lastAppResult->duration);
			ImGui::Text("Ran in: %s", std::format("{:%T}", elapsedMs).c_str());
			ImGui::Text("Consumed %u frames", lastAppResult->nFramesConsumed);
			SptImGui::EndBordered();
		}
	}

	ImGui::BeginDisabled(jobRunning);
	if (ImGui::Button("Start rendering"))
	{
		// TODO this is a duplicate from below
		if (ArPlaceholder::cmdLineFormattedDirty.exchange(false))
		{
			ArPlaceholders::FormatCmdLine(persist.cmdLineUnformatted,
			                              persist.cmdLineFormatted,
			                              &persist.unrecognizedPlaceholders);
		}

		auto result = std::make_unique<ArAppResult>();
		auto newDeferredMovieJob = std::unique_ptr<ArDeferredMovieJob>(new ArDeferredMovieJob{
		    .args{
		        .ffmpegWorkingDir = ArUtf8ToUtf16(ArPlaceholders::RENDER_WORKING_DIR.GetValue()->c_str()),
		        .cmd = ArUtf8ToUtf16(persist.cmdLineFormatted.c_str()),
		        .videoPipeName = ArUtf8ToUtf16(ArPlaceholders::VIDEO_PIPE_NAME.GetValue()->c_str()),
		        .audioPipeName = ArUtf8ToUtf16(ArPlaceholders::AUDIO_PIPE_NAME.GetValue()->c_str()),
		        .width = (size_t)atoi(ArPlaceholders::VID_WIDTH.GetValue()->c_str()),
		        .height = (size_t)atoi(ArPlaceholders::VID_HEIGHT.GetValue()->c_str()),
		        .ffmpegReturnCode = result->returnCode,
		    },
		    .result = std::move(result),
		});
		// TODO do I have to lock for this? Can I just use an atomic
		std::lock_guard lk(jobMtx);
		std::swap(deferredMovieJob, newDeferredMovieJob);
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(!jobRunning);
	if (ImGui::Button("Stop rendering"))
	{
		std::lock_guard lk(jobMtx);
		if (runningMovieJob)
			runningMovieJob->killSignal = true;
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	if (ImGui::Button("Open log folder"))
	{
		std::wstring wWorkingDirStr = ArUtf8ToUtf16(ArPlaceholders::RENDER_WORKING_DIR.GetValue()->c_str());
		if (!wWorkingDirStr.empty())
		{
			std::filesystem::path workingDir = wWorkingDirStr;
			std::error_code ec;
			std::filesystem::create_directories(workingDir, ec);
			ShellExecuteW(NULL, L"open", wWorkingDirStr.c_str(), NULL, NULL, SW_SHOWDEFAULT);
		}
	}

	ImGui::BeginDisabled(jobRunning);

	// ffmpeg path

	if (!persist.lastFfmpegSearchSuccess.has_value())
		persist.lastFfmpegSearchSuccess = ArPlaceholders::FindFFmpeg();

	{
		static std::string tmp;
		tmp = *ArPlaceholders::EXE_PATH.GetValue();
		if (ImGui::InputText("Exe path", &tmp))
		{
			ArPlaceholder::cmdLineFormattedDirty.store(true);
			ArPlaceholders::EXE_PATH.SetValue(tmp);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Auto-detect"))
		persist.lastFfmpegSearchSuccess = ArPlaceholders::FindFFmpeg();
	ImGui::SetItemTooltip("search for ffmpeg in the PATH");
	ImGui::SameLine();
	SptImGui::HelpMarker(
	    "This is the application that all data will be pumped into.\n"
	    "By default it's ffmpeg, but you can in theory use any application\n"
	    "that accepts named pipes as input for raw video and audio streams.");

	if (!persist.lastFfmpegSearchSuccess.value())
	{
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "Warning: ffmpeg.exe not found,");
		ImGui::SameLine();
		ImGui::TextLinkOpenURL("download it", "https://www.gyan.dev/ffmpeg/builds/ffmpeg-git-full.7z");
		ImGui::SameLine();
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "and add it to your PATH.");
	}

	// pipe name option(s)

	int pipeOpt = !!appendUuidToPipes;
	const char* pipeNameOpts[] = {"Keep name consistent", "Append UUID (default)"};
	if (ImGui::Combo("Pipe name", &pipeOpt, pipeNameOpts, ARRAYSIZE(pipeNameOpts)))
	{
		appendUuidToPipes = !!pipeOpt;
		ArPlaceholders::SetPipeNames();
	}
	ImGui::SameLine();
	SptImGui::HelpMarker(
	    "Advanced users only!\n"
	    "By default, a random name is used for the video/audio pipes for every video.\n"
	    "This allows rendering with multiple instances and minimizes bugs if this code fails to close ffmpeg correctly.\n"
	    "This option is mostly for debugging.");

	// framerate

	if (ImGui::InputFloat("Framerate", &persist.fpsVal))
	{
		persist.fpsVal = clamp(persist.fpsVal, 0.001f, 100'000);
		ArPlaceholders::FRAMERATE.SetValue(std::to_string(persist.fpsVal));
	}

	// TODO - option for different videohook
	// TODO - option for rendering demos or number of frames

	// async mode

	ImGui::TextUnformatted("Sync mode:");
	ImGui::SameLine();
	ImGui::RadioButton("Synchronous", (int*)&persist.syncMode, AR_SYNC_FULL);
	ImGui::SameLine();
	ImGui::RadioButton("Async", (int*)&persist.syncMode, AR_SYNC_ASYNC);
	ImGui::SameLine();
	ImGui::RadioButton("Threaded", (int*)&persist.syncMode, AR_SYNC_THREADED);

	ImGui::BeginDisabled(persist.syncMode == AR_SYNC_FULL);
	ImGui::SliderInt("Number of frames in flight",
	                 &persist.nFramesInFlight,
	                 1,
	                 3,
	                 nullptr,
	                 ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();

	ImGui::SameLine();
	SptImGui::HelpMarker(
	    "Determines how frames are submitted to the app:\n"
	    " - Synchronous: frames are pumped in on the render thread\n"
	    " - Async: frames are asynchronously requested from the GPU and pumped in a few frames later\n"
	    " - Threaded: frames are asynchronously requested from the GPU and pumped in by a worker thread\n"
	    "\n"
	    "The default threaded setting should be the fastest - the other options exist as a slower fallback.");

	// cmdline TODO: wrap

	if (persist.resetCmdLine)
	{
		persist.cmdLineUnformatted = ArPlaceholders::CreateDefaultCmdLine();
		persist.resetCmdLine = false;
		ArPlaceholder::cmdLineFormattedDirty.store(true);
	}

	ImGui::TextUnformatted("Program to execute:");
	if (ImGui::InputTextMultiline("##cmdline_unformatted",
	                              &persist.cmdLineUnformatted,
	                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 16)))
	{
		ArPlaceholder::cmdLineFormattedDirty.store(true);
	}

	if (ImGui::Button("Reset to default"))
	{
		persist.cmdLineUnformatted = ArPlaceholders::CreateDefaultCmdLine();
		ArPlaceholder::cmdLineFormattedDirty.store(true);
	}

	// placeholder info

	if (!persist.unrecognizedPlaceholders.empty())
	{
		std::string out = "Warning: unrecognized placeholders: ";
		for (auto& s : persist.unrecognizedPlaceholders)
		{
			out += ArPlaceholder::UnformattedStr(s);
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
			for (const ArPlaceholder& pl : ArPlaceholder::GetAll())
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(pl.UnformattedKey().c_str());
				ImGui::TableNextColumn();
				auto val = pl.GetValue();
				ImGui::TextUnformatted(val ? val->c_str() : "-");
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(pl.helpText);
			}
			ImGui::EndTable();
		}

		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Formatted command"))
	{
		// don't reformat the string if we're in a disabled block - that might confuse the user
		if (!jobRunning && ArPlaceholder::cmdLineFormattedDirty.exchange(false))
		{
			ArPlaceholders::FormatCmdLine(persist.cmdLineUnformatted,
			                              persist.cmdLineFormatted,
			                              &persist.unrecognizedPlaceholders);
		}

		ImGui::InputTextMultiline("##cmdline_formated",
		                          &persist.cmdLineFormatted,
		                          ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 16),
		                          ImGuiInputTextFlags_ReadOnly);
		ImGui::TreePop();
	}

	ImGui::EndDisabled();
}

bool ArPlaceholders::FindFFmpeg()
{
	wchar wExePath[MAX_PATH];
	DWORD wlen = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, wExePath, nullptr);
	if (wlen >= MAX_PATH || wlen == 0)
		return false;
	char exePath[MAX_PATH];
	BOOL usedDefault;
	DWORD len = WideCharToMultiByte(CP_UTF8,
	                                WC_ERR_INVALID_CHARS,
	                                wExePath,
	                                wlen,
	                                exePath,
	                                MAX_PATH,
	                                nullptr,
	                                &usedDefault);
	if (len == 0 || len >= MAX_PATH || usedDefault)
		return false;

	EXE_PATH.SetValue(std::string(exePath, len));
	return true;
}

std::string ArPlaceholders::CreateDefaultCmdLine()
{
	return std::format(
	    "\"{}\" -report -f rawvideo -pixel_format bgr0 -video_size {}x{} -framerate {} -i \"{}\" -y -c:v libx264 -preset veryfast -crf 18 \"{}\\my_video.mp4\"",
	    ArPlaceholders::EXE_PATH.UnformattedKey(),
	    ArPlaceholders::VID_WIDTH.UnformattedKey(),
	    ArPlaceholders::VID_HEIGHT.UnformattedKey(),
	    ArPlaceholders::FRAMERATE.UnformattedKey(),
	    ArPlaceholders::VIDEO_PIPE_NAME.UnformattedKey(),
	    ArPlaceholders::RENDER_WORKING_DIR.UnformattedKey());
}

void ArPlaceholders::FormatCmdLine(const std::string& unformatted,
                                   std::string& formatted,
                                   std::vector<std::string>* unrecognizedPlaceholders)
{
	formatted.clear();
	if (unrecognizedPlaceholders)
		unrecognizedPlaceholders->clear();

	bool inPlaceholder = false;
	std::string placeholder;

	for (char c : unformatted)
	{
		if (isspace(c))
			c = ' ';
		if (c == '{' && inPlaceholder)
		{
			// what we have so far is not a real placeholder - flush as raw string
			formatted += '{';
			formatted += placeholder;
			placeholder.clear();
			inPlaceholder = c == '{';
		}
		else if (c == '{' && !inPlaceholder)
		{
			inPlaceholder = true;
		}
		else if (c == '}' && inPlaceholder)
		{
			// actual placeholder - check if it matches anything we have
			auto& allPlaceHolders = ArPlaceholder::GetAll();
			auto it = std::ranges::find(allPlaceHolders, placeholder, [](auto& p) { return p.get().key; });
			auto placeHolderVal = it == allPlaceHolders.cend() ? nullptr : it->get().GetValue();

			if (placeHolderVal)
			{
				formatted += *placeHolderVal;
			}
			else
			{
				formatted += '{';
				formatted += placeholder;
				formatted += '}';
				/*
				* If we didn't find a match by key - this is not a valid placeholder. If it has a
				* key but no value, we expect for it to be substituted later.
				*/
				if (it == allPlaceHolders.cend() && unrecognizedPlaceholders)
					unrecognizedPlaceholders->push_back(std::move(placeholder));
			}
			placeholder.clear();
			inPlaceholder = false;
		}
		else if (inPlaceholder)
		{
			placeholder += c;
		}
		else
		{
			formatted += c;
		}
	}

	if (inPlaceholder)
	{
		// flush as raw string
		formatted += '{';
		formatted += placeholder;
	}
}

void ArPlaceholders::RegenerateUuid()
{
	::UUID uuid;
	bool err = UuidCreate(&uuid) != RPC_S_OK;
	char* uuidStr = nullptr;
	if (!err)
		err = UuidToStringA(&uuid, (RPC_CSTR*)&uuidStr) != RPC_S_OK;
	if (err)
	{
		// randomly chosen UUID by way of dice
		UUID.SetValue("00000000-0000-0000-0000-000000000000");
	}
	else
	{
		UUID.SetValue(uuidStr);
		RpcStringFreeA((RPC_CSTR*)&uuidStr);
	}
}

void ArPlaceholders::SetPipeNames()
{
	const char* videoPipeName = R"(\\.\pipe\spt_autorender_video)";
	const char* audioPipeName = R"(\\.\pipe\spt_autorender_audio)";

	if (spt_auto_render_feat.appendUuidToPipes)
	{
		auto uuid = UUID.GetValue();
		ArPlaceholders::VIDEO_PIPE_NAME.SetValue(std::format("{}_{}", videoPipeName, uuid->c_str()));
		ArPlaceholders::AUDIO_PIPE_NAME.SetValue(std::format("{}_{}", audioPipeName, uuid->c_str()));
	}
	else
	{
		ArPlaceholders::VIDEO_PIPE_NAME.SetValue(videoPipeName);
		ArPlaceholders::AUDIO_PIPE_NAME.SetValue(audioPipeName);
	}
}

IMPL_HOOK_CDECL(AutoRenderFeature,
                void,
                S_TransferStereo16,
                void* pOutput,
                const portable_samplepair_t* pfront,
                int lpaintedtime,
                int endtime)
{
	ORIG_S_TransferStereo16(pOutput, pfront, lpaintedtime, endtime);

	// try to replicate the game's logic here

	short* snd_out;
	int* snd_p = (int*)pfront;
	int snd_linear_count;

	constexpr int deviceSampleCount = 44100;
	int samplePairCount = deviceSampleCount >> 1;
	int sampleMask = samplePairCount - 1;

	while (lpaintedtime < endtime)
	{
		int lpos = lpaintedtime & sampleMask;
		snd_out = (short*)pOutput + (lpos << 1);
		snd_linear_count = std::min(samplePairCount - lpos, samplePairCount - lpos);
		snd_linear_count <<= 1;
		snd_p += snd_linear_count;
		lpaintedtime += snd_linear_count >> 1;
	}
}
