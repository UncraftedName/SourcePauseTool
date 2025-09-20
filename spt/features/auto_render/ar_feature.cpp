#include "stdafx.hpp"

#include "ar_decls.hpp"
#include "ar_util.hpp"
#include "spt/feature.hpp"
#include "spt/features/visualizations/imgui/imgui_interface.hpp"

#include <variant>
#include <chrono>
#include <mutex>

enum ArSyncMode
{
	AR_SYNC_FULL,
	AR_SYNC_ASYNC,
	AR_SYNC_THREADED,
};

struct ArDeferredJob
{
	struct TgaScreenshot
	{
		std::string path;
		bool openAfterWrite;
	};

	struct TgaExport
	{
		size_t nFrames;
		std::string pathFmt;
	};

	struct FfmpegExport
	{
		size_t nFrames;
		std::string cmd;
	};

	struct LockAndDiscard
	{
		size_t nFrames;
	};

	struct StopAll
	{
	};

	std::variant<TgaScreenshot, TgaExport, FfmpegExport, LockAndDiscard, StopAll> jobType;
	ArSyncMode syncMode;
	size_t nFramesInFlight; // only used if asyncMode == AR_SYNC_ASYNC
};

struct ArRunningJob
{
	using clock = std::chrono::high_resolution_clock;

	clock::time_point startTime;
	std::optional<ar_frame_idx> maxConsumeFrames;
	std::unique_ptr<ArSyncManager> mgr;

	void PrintTimeStats() const
	{
		auto curTime = clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::microseconds>(curTime - startTime).count();

		size_t nConsumedFrames = mgr->GetNumConsumedFrames();
		Msg("SPT: finished job, consumed %u frames in %.3fs (avg %.3fms/frame)\n\n",
		    nConsumedFrames,
		    duration / 1e6f,
		    duration / 1e3f / nConsumedFrames);
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

#define AR_UNFMT_PLCH(x) "{" x "}"
#define AR_UNFMT_PLCH_QUOTED(x) "\"{" x "}\""
#define AR_UNFMT_PLCH_STD_STRING(x) "{" + x + "}"

#define AR_PLCH_EXE_PATH "EXE_PATH"
#define AR_PLCH_VIDEO_DIMENSIONS "DIMENSIONS"
#define AR_PLCH_FRAMERATE "FPS"
#define AR_PLCH_VIDEO_PIPE_NAME "VIDEO_PIPE"
#define AR_PLCH_AUDIO_PIPE_NAME "AUDIO_PIPE"
#define AR_PLCH_OUTPUT "OUTPUT_PATH"

class AutoRenderFeature : public FeatureWrapper<AutoRenderFeature>
{
public:
	struct Jobs
	{
		static inline std::recursive_mutex mtx;
		std::vector<ArDeferredJob> deferred;
		std::vector<ArRunningJob> running;
	} jobs;

protected:
	virtual void InitHooks() override;
	virtual void LoadFeature() override;
	virtual void UnloadFeature() override;

private:
	void OnShaderDevicePresentSignal(IDirect3DDevice9* device);
	void ImGuiTabCallback();
	// TODO move into the persist struct
	void FindFFmpeg();
	void ResetUnformattedCmdLine();
	void FormatCmdLine();

	/*
	* All strings here are stored as utf8 to display in ImGui.
	* TODO - reset on unload
	*/
	struct Persist
	{
		static inline std::mutex mtx;

		bool searchedForExe = false;
		bool lastSearchSuccess = false;
		bool setDefaultCmdLine = false;
		bool cmdLineFormattedDirty = true;
		std::string cmdLineUnformatted;
		std::unordered_map<std::string, std::string> placeholders;
		std::set<std::string> unrecognizedPlaceholders;
		std::string cmdLineFormatted;
	} persist;

	DECL_STATIC_HOOK_CDECL(void,
	                       S_TransferStereo16,
	                       void* pOutput,
	                       const portable_samplepair_t* pfront,
	                       int lpaintedtime,
	                       int endtime);

} static spt_auto_render_feat;

CON_COMMAND_F(spt_ar_screenshot, "screenshot [path] <open> <async_mode>", FCVAR_DONTRECORD)
{
	if (args.ArgC() < 2)
	{
		Warning("Usage: %s\n", spt_ar_screenshot_command.GetHelpText());
		return;
	}

	ArDeferredJob::TgaScreenshot jobType{
	    .path = args.Arg(1),
	    .openAfterWrite = args.ArgC() >= 3 ? !!atoi(args.Arg(2)) : false,
	};
	ArDeferredJob job{
	    .jobType = jobType,
	    .syncMode = args.ArgC() >= 4 ? (ArSyncMode)atoi(args.Arg(3)) : AR_SYNC_FULL,
	    .nFramesInFlight = 1,
	};

	std::lock_guard lock(AutoRenderFeature::Jobs::mtx);
	spt_auto_render_feat.jobs.deferred.push_back(job);
}

CON_COMMAND_F(spt_ar_profile, "Profile [async_mode 0-2] [n_frames] <num_frames_in_flight>", FCVAR_DONTRECORD)
{
	if (args.ArgC() < 3)
	{
		Warning("Usage: %s\n", spt_ar_profile_command.GetHelpText());
		return;
	}

	auto asyncMode = (ArSyncMode)atoi(args.Arg(1));
	if ((asyncMode == AR_SYNC_ASYNC || asyncMode == AR_SYNC_THREADED) && args.ArgC() < 4)
	{
		Warning("Must specify a number of frames in flight for mode %d\n", asyncMode);
		return;
	}

	ArDeferredJob::LockAndDiscard jobType{
	    .nFrames = (size_t)atoi(args.Arg(2)),
	};
	ArDeferredJob job{
	    .jobType = jobType,
	    .syncMode = asyncMode,
	    .nFramesInFlight = args.ArgC() >= 4 ? (size_t)atoi(args.Arg(3)) : 0,
	};

	std::lock_guard lock(AutoRenderFeature::Jobs::mtx);
	spt_auto_render_feat.jobs.deferred.push_back(job);

	Msg("Spinning up dummy consumers...\n");
}

CON_COMMAND_F(spt_ar_startmovie,
              "startmovie [path_fmt] [async_mode 0-2] [n_frames] <num_frames_in_flight>",
              FCVAR_DONTRECORD)
{
	if (args.ArgC() < 4)
	{
		Warning("Usage: %s\n", spt_ar_startmovie_command.GetHelpText());
		return;
	}
	auto asyncMode = (ArSyncMode)atoi(args.Arg(2));
	if ((asyncMode == AR_SYNC_ASYNC || asyncMode == AR_SYNC_THREADED) && args.ArgC() < 5)
	{
		Warning("Must specify a number of frames in flight for mode %d\n", asyncMode);
		return;
	}
	ArDeferredJob::TgaExport jobType{
	    .nFrames = (size_t)atoi(args.Arg(3)),
	    .pathFmt = args.Arg(1),
	};
	ArDeferredJob job{
	    .jobType = jobType,
	    .syncMode = asyncMode,
	    .nFramesInFlight = args.ArgC() >= 5 ? (size_t)atoi(args.Arg(4)) : 0,
	};
	std::lock_guard lock(AutoRenderFeature::Jobs::mtx);
	spt_auto_render_feat.jobs.deferred.push_back(job);
}

CON_COMMAND_F(spt_ar_startmovie_ffmpeg,
              "startmovie [output_path] [async_mode 0-2] [n_frames] <num_frames_in_flight>",
              FCVAR_DONTRECORD)
{
	if (args.ArgC() < 4)
	{
		Warning("Usage: %s\n", spt_ar_startmovie_command.GetHelpText());
		return;
	}
	auto asyncMode = (ArSyncMode)atoi(args.Arg(2));
	if ((asyncMode == AR_SYNC_ASYNC || asyncMode == AR_SYNC_THREADED) && args.ArgC() < 5)
	{
		Warning("Must specify a number of frames in flight for mode %d\n", asyncMode);
		return;
	}

	ArDeferredJob::FfmpegExport jobType{
	    .nFrames = (size_t)atoi(args.Arg(3)),
	    .cmd = std::format(
	        "\"{}\" -y -report -f rawvideo -pixel_format bgr0 -video_size {} -framerate 60 -i \"{}\" -c:v libx264 \"{}\"",
	        "{FFMPEG_PATH}",
	        "{WIDTH}x{HEIGHT}",
	        "{VIDEO_PIPE_NAME}",
	        args.Arg(1)),
	};
	ArDeferredJob job{
	    .jobType = jobType,
	    .syncMode = asyncMode,
	    .nFramesInFlight = args.ArgC() >= 5 ? (size_t)atoi(args.Arg(4)) : 0,
	};
	std::lock_guard lock(AutoRenderFeature::Jobs::mtx);
	spt_auto_render_feat.jobs.deferred.push_back(job);
}

CON_COMMAND_F(spt_ar_stop_all, "Stop all tasks", FCVAR_DONTRECORD)
{
	std::lock_guard lock(AutoRenderFeature::Jobs::mtx);
	spt_auto_render_feat.jobs.deferred.push_back(ArDeferredJob{.jobType = ArDeferredJob::StopAll{}});
}

void AutoRenderFeature::InitHooks()
{
	HOOK_FUNCTION(engine, S_TransferStereo16);
}

void AutoRenderFeature::LoadFeature()
{
	if (!ShaderDevicePresentSignal.Works || !SptImGui::Loaded())
		return;

	ShaderDevicePresentSignal.Connect(this, &AutoRenderFeature::OnShaderDevicePresentSignal);
	InitCommand(spt_ar_screenshot);
	InitCommand(spt_ar_profile);
	InitCommand(spt_ar_stop_all);
	InitCommand(spt_ar_startmovie);
	InitCommand(spt_ar_startmovie_ffmpeg);

	SptImGuiGroup::QoL_AutoRender.RegisterUserCallback([this]() { ImGuiTabCallback(); });
}

void AutoRenderFeature::UnloadFeature()
{
	{
		std::lock_guard lock(AutoRenderFeature::Jobs::mtx);
		AutoRenderFeature::Jobs newJobs{};
		std::swap(newJobs, jobs);
	}
	{
		std::lock_guard lock(AutoRenderFeature::Persist::mtx);
		AutoRenderFeature::Persist newPersist{};
		std::swap(newPersist, persist);
	}
}

// TODO ewwww
bool replaceGross(std::string& str, const std::string& from, const std::string& to)
{
	size_t start_pos = str.find(from);
	if (start_pos == std::string::npos)
		return false;
	str.replace(start_pos, from.length(), to);
	return true;
}

// helper for std::visit
template<class... Ts>
struct overloaded : Ts...
{
	using Ts::operator()...;
};

void AutoRenderFeature::OnShaderDevicePresentSignal(IDirect3DDevice9* device)
{
	std::lock_guard lock(AutoRenderFeature::Jobs::mtx);

	// stop all running jobs up to the stop all job (but not any new jobs after that)
	size_t nJobsToStop = 0;

	static std::optional<DWORD> ffmpegReturnVal;

	for (auto& defferedJob : jobs.deferred)
	{
		auto startTime = ArRunningJob::clock::now(); // factor in setup time

		// create the consumer

		std::unique_ptr<ArLockableSurfaceConsumer> consumer;
		std::optional<ar_frame_idx> nFrames;
		ser::StatusTracker stat;

		std::visit(
		    overloaded{
		        [&](ArDeferredJob::TgaScreenshot& desc)
		        {
			        consumer =
			            std::make_unique<ArTgaWriter>(std::move(desc.path), true, desc.openAfterWrite);
			        nFrames = 1;
		        },
		        [&](ArDeferredJob::TgaExport& desc)
		        {
			        consumer = std::make_unique<ArTgaWriter>(std::move(desc.pathFmt), false, false);
			        nFrames = desc.nFrames;
		        },
		        [&](ArDeferredJob::FfmpegExport& desc)
		        {
			        auto [_, backDesc] = ArGetBackBufferInfo(device, stat);
			        if (!stat.Ok())
				        return;
			        std::string exePath = R"(C:\Users\UncraftedName\scoop\shims\ffmpeg.exe)";
			        const char* videoPipeName = R"(\\.\pipe\spt_ffmpeg_video)";

			        ffmpegReturnVal.reset();

			        replaceGross(desc.cmd, "{FFMPEG_PATH}", exePath);
			        replaceGross(desc.cmd, "{WIDTH}", std::to_string(backDesc.Width));
			        replaceGross(desc.cmd, "{HEIGHT}", std::to_string(backDesc.Height));
			        replaceGross(desc.cmd, "{VIDEO_PIPE_NAME}", videoPipeName);
			        ArFfmpegWriter::InitArgs args{
			            .ffmpegWorkingDir = "spt_autorender",
			            .cmd = desc.cmd.data(),
			            .videoPipeName = videoPipeName,
			            .width = backDesc.Width,
			            .height = backDesc.Height,
			            .ffmpegReturnCode = ffmpegReturnVal,
			        };
			        consumer = std::make_unique<ArFfmpegWriter>(args, stat);
			        nFrames = desc.nFrames;
		        },
		        [&](ArDeferredJob::LockAndDiscard& desc)
		        {
			        consumer = std::make_unique<ArNullConsumer>();
			        nFrames = desc.nFrames;
		        },
		        [&](ArDeferredJob::StopAll&) { nJobsToStop = jobs.running.size(); },
		    },
		    defferedJob.jobType);

		if (!stat.Ok())
		{
			Warning("Error while creating consumer: %s\n", stat.GetStatus().errMsg.c_str());
			continue;
		}

		if (!consumer)
			continue;

		// create the consumer manager

		std::unique_ptr<ArSyncManager> mgr;

		switch (defferedJob.syncMode)
		{
		case AR_SYNC_FULL:
			mgr = std::make_unique<ArSynchronousConsumerManager>(device,
			                                                     D3DFMT_A8R8G8B8,
			                                                     std::move(consumer),
			                                                     stat);
			break;
		case AR_SYNC_ASYNC:
			mgr = std::make_unique<ArAsyncConsumerManager>(device,
			                                               D3DFMT_A8R8G8B8,
			                                               defferedJob.nFramesInFlight,
			                                               std::move(consumer),
			                                               stat);

			break;
		case AR_SYNC_THREADED:
			mgr = std::make_unique<ArThreadedConsumerManager>(device,
			                                                  D3DFMT_A8R8G8B8,
			                                                  defferedJob.nFramesInFlight,
			                                                  std::move(consumer),
			                                                  stat);
			break;
		default:
			stat.Err(std::format("Unrecognized sync mode {}", (int)defferedJob.syncMode));
		}
		if (!stat.Ok())
		{
			Warning("Error while creating consumer manager: %s\n", stat.GetStatus().errMsg.c_str());
			continue;
		}
		jobs.running.emplace_back(startTime, nFrames, std::move(mgr));
	}
	jobs.deferred.clear();

	// technically I don't think we should be calling Msg/Warning from the render thread :/

	// submit frames to running jobs

	size_t n = 0;
	for (auto it = jobs.running.begin(); it != jobs.running.end(); n++)
	{
		auto& job = *it;
		ser::StatusTracker stat;

		bool shouldStop =
		    n < nJobsToStop
		    || (job.maxConsumeFrames.has_value() && job.mgr->GetNumConsumedFrames() >= job.maxConsumeFrames);

		if (!shouldStop)
		{
			job.mgr->OnDevicePresent(device, stat);
			if (!stat.Ok())
			{
				Warning("Error submitting frame to job: %s\n", stat.GetStatus().errMsg.c_str());
				jobs.running.erase(it++);
				continue;
			}
		}

		if (shouldStop)
		{
			job.mgr->Finish(device, stat);
			if (stat.Ok())
				jobs.running.front().PrintTimeStats();
			else
				Warning("Error while stopping job: %s\n", stat.GetStatus().errMsg.c_str());
			it = jobs.running.erase(it++);
			continue;
		}

		++it;
	}
}

void AutoRenderFeature::ImGuiTabCallback()
{
	// ffmpeg input

	if (!persist.searchedForExe)
	{
		FindFFmpeg();
		persist.searchedForExe = true;
	}

	ImGui::TextUnformatted("exe path:");
	ImGui::SameLine();
	auto& exePath = persist.placeholders[AR_PLCH_EXE_PATH];
	ImGui::InputText("##exe_input", exePath.data(), exePath.capacity());
	ImGui::SameLine();
	if (ImGui::Button("Auto-detect"))
		FindFFmpeg();
	ImGui::SetItemTooltip("search for ffmpeg in the PATH");
	ImGui::SameLine();
	SptImGui::HelpMarker(
	    "This is the application that all data will be pumped into.\n"
	    "By default it's ffmpeg, but you can in theory use any application\n"
	    "that accepts named pipes as input for raw video and audio streams.");

	if (!persist.lastSearchSuccess)
	{
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "Warning: ffmpeg.exe not found,");
		ImGui::SameLine();
		ImGui::TextLinkOpenURL("download it", "https://www.gyan.dev/ffmpeg/builds/ffmpeg-git-full.7z");
		ImGui::SameLine();
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "and add it to your PATH.");
	}

	// cmdline TODO: wrap

	if (!persist.setDefaultCmdLine)
	{
		ResetUnformattedCmdLine();
		persist.setDefaultCmdLine = true;
		persist.cmdLineFormattedDirty = true;
	}

	ImGui::TextUnformatted("Program to execute:");

	// resize the unformatted string if it the user gives us a huge cmd

	auto resizeCallback = [](ImGuiInputTextCallbackData* data) -> int
	{
		if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
		{
			std::string* str = (std::string*)data->UserData;
			str->resize(data->BufSize);
			data->Buf = str->data();
		}
		return 0;
	};

	persist.cmdLineFormattedDirty |= ImGui::InputTextMultiline("##cmdline_unformatted",
	                                                           persist.cmdLineUnformatted.data(),
	                                                           persist.cmdLineUnformatted.capacity(),
	                                                           ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 16),
	                                                           ImGuiInputTextFlags_None,
	                                                           resizeCallback,
	                                                           &persist.cmdLineUnformatted);
	if (ImGui::Button("Reset to default"))
	{
		ResetUnformattedCmdLine();
		persist.cmdLineFormattedDirty = true;
	}

	if (persist.cmdLineFormattedDirty)
	{
		FormatCmdLine();
		persist.cmdLineFormattedDirty = false;
	}

	if (!persist.unrecognizedPlaceholders.empty())
	{
		std::string out = "Warning, unrecognized placeholders: ";
		for (auto& s : persist.unrecognizedPlaceholders)
			out += AR_UNFMT_PLCH_STD_STRING(s) + ", ";
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "%.*s", out.size() - 2, out.c_str());
	}

	if (ImGui::TreeNode("Formatted command"))
	{
		ImGui::InputTextMultiline("##cmdline_formated",
		                          persist.cmdLineFormatted.data(),
		                          persist.cmdLineFormatted.size(),
		                          ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 16),
		                          ImGuiInputTextFlags_ReadOnly);
		ImGui::TreePop();
	}
}

void AutoRenderFeature::FindFFmpeg()
{
	persist.cmdLineFormattedDirty = true;

	auto [it, _] = persist.placeholders.try_emplace(AR_PLCH_EXE_PATH, "C:\\YOUR_MOM\\ffmpeg.exe");
	auto& exePath = it->second;
	exePath.reserve(MAX_PATH);

	wchar wExePath[MAX_PATH];
	DWORD wlen = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, wExePath, nullptr);
	if (wlen >= MAX_PATH || wlen == 0)
	{
		persist.lastSearchSuccess = false;
		return;
	}
	BOOL usedDefault;
	DWORD len = WideCharToMultiByte(CP_UTF8,
	                                WC_ERR_INVALID_CHARS,
	                                wExePath,
	                                wlen,
	                                exePath.data(),
	                                exePath.size(),
	                                nullptr,
	                                &usedDefault);
	if (len == 0 || len >= sizeof(exePath.capacity()) || usedDefault)
	{
		persist.lastSearchSuccess = false;
		return;
	}

	persist.lastSearchSuccess = true;
}

void AutoRenderFeature::ResetUnformattedCmdLine()
{
	persist.cmdLineUnformatted = 
		AR_UNFMT_PLCH_QUOTED(AR_PLCH_EXE_PATH) " -report "
	    "-f rawvideo -pixel_format bgr0 -video_size " AR_UNFMT_PLCH(AR_PLCH_VIDEO_DIMENSIONS) " "
		"-framerate " AR_UNFMT_PLCH(AR_PLCH_FRAMERATE) " -i " AR_UNFMT_PLCH_QUOTED(AR_PLCH_VIDEO_PIPE_NAME) " "
	    "-y -c:v libx264 " AR_UNFMT_PLCH_QUOTED(AR_PLCH_OUTPUT);
}

void AutoRenderFeature::FormatCmdLine()
{
	auto& out = persist.cmdLineFormatted;
	out.clear();
	persist.unrecognizedPlaceholders.clear();

	bool inPlaceholder = false;
	std::string placeholder;

	for (char c : persist.cmdLineUnformatted)
	{
		if (c == '{' && inPlaceholder)
		{
			// what we have so far is not a real placeholder - flush as raw string
			out += '{';
			out += placeholder;
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
			auto it = persist.placeholders.find(placeholder);
			if (it == persist.placeholders.cend())
			{
				persist.unrecognizedPlaceholders.insert(placeholder);
				out += '{';
				out += placeholder;
				out += '}';
			}
			else
			{
				out += it->second;
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
			out += c;
		}
	}

	if (inPlaceholder)
	{
		// flush as raw string
		out += '{';
		out += placeholder;
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
