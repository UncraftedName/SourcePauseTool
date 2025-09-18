#include "stdafx.hpp"

#include "ar_decls.hpp"
#include "ar_util.hpp"
#include "spt/feature.hpp"

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

class AutoRenderFeature : public FeatureWrapper<AutoRenderFeature>
{
public:
	static inline std::recursive_mutex jobMtx;
	std::vector<ArDeferredJob> deferredJobs;
	std::list<ArRunningJob> runningJobs;

protected:
	virtual void InitHooks() override;
	virtual void LoadFeature() override;
	virtual void UnloadFeature() override;

private:
	void OnShaderDevicePresentSignal(IDirect3DDevice9* device);

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

	std::lock_guard lock(AutoRenderFeature::jobMtx);
	spt_auto_render_feat.deferredJobs.push_back(job);
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

	std::lock_guard lock(AutoRenderFeature::jobMtx);
	spt_auto_render_feat.deferredJobs.push_back(job);

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
	std::lock_guard lock(AutoRenderFeature::jobMtx);
	spt_auto_render_feat.deferredJobs.push_back(job);
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
	        "\"{}\" -y -f rawvideo -pixel_format bgr0 -video_size {} -framerate 60 -i pipe:0 -c:v libx264 \"{}\"",
	        "{FFMPEG_PATH}",
	        "{WIDTH}x{HEIGHT}",
	        args.Arg(1)),
	};
	ArDeferredJob job{
	    .jobType = jobType,
	    .syncMode = asyncMode,
	    .nFramesInFlight = args.ArgC() >= 5 ? (size_t)atoi(args.Arg(4)) : 0,
	};
	std::lock_guard lock(AutoRenderFeature::jobMtx);
	spt_auto_render_feat.deferredJobs.push_back(job);
}

CON_COMMAND_F(spt_ar_stop_all, "Stop all tasks", FCVAR_DONTRECORD)
{
	std::lock_guard lock(AutoRenderFeature::jobMtx);
	spt_auto_render_feat.deferredJobs.push_back(ArDeferredJob{.jobType = ArDeferredJob::StopAll{}});
}

void AutoRenderFeature::InitHooks() {}

void AutoRenderFeature::LoadFeature()
{
	if (!ShaderDevicePresentSignal.Works)
		return;

	ShaderDevicePresentSignal.Connect(this, &AutoRenderFeature::OnShaderDevicePresentSignal);
	InitCommand(spt_ar_screenshot);
	InitCommand(spt_ar_profile);
	InitCommand(spt_ar_stop_all);
	InitCommand(spt_ar_startmovie);
	InitCommand(spt_ar_startmovie_ffmpeg);
}

void AutoRenderFeature::UnloadFeature()
{
	std::lock_guard lock(AutoRenderFeature::jobMtx);
	deferredJobs.clear();
	runningJobs.clear();
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
	std::lock_guard lock(AutoRenderFeature::jobMtx);

	// stop all running jobs up to the stop all job (but not any new jobs after that)
	size_t nJobsToStop = 0;

	for (auto& defferedJob : deferredJobs)
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
			        std::string ffmpegPath = R"(C:\Users\uncra\scoop\shims\ffmpeg.exe)";
			        replaceGross(desc.cmd, "{FFMPEG_PATH}", ffmpegPath);
			        replaceGross(desc.cmd, "{WIDTH}", std::to_string(backDesc.Width));
			        replaceGross(desc.cmd, "{HEIGHT}", std::to_string(backDesc.Height));
			        consumer = std::make_unique<ArFfmpegWriter>(ffmpegPath, desc.cmd, stat);
			        nFrames = desc.nFrames;
		        },
		        [&](ArDeferredJob::LockAndDiscard& desc)
		        {
			        consumer = std::make_unique<ArNullConsumer>();
			        nFrames = desc.nFrames;
		        },
		        [&](ArDeferredJob::StopAll&) { nJobsToStop = runningJobs.size(); },
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
		runningJobs.emplace_back(startTime, nFrames, std::move(mgr));
	}
	deferredJobs.clear();

	// technically I don't think we should be calling Msg/Warning from the render thread :/

	// submit frames to running jobs

	size_t n = 0;
	for (auto it = runningJobs.begin(); it != runningJobs.end(); n++)
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
				runningJobs.erase(it++);
				continue;
			}
		}

		if (shouldStop)
		{
			job.mgr->Finish(device, stat);
			if (stat.Ok())
				runningJobs.front().PrintTimeStats();
			else
				Warning("Error while stopping job: %s\n", stat.GetStatus().errMsg.c_str());
			it = runningJobs.erase(it++);
			continue;
		}

		++it;
	}
}
