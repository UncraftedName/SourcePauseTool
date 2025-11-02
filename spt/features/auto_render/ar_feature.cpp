#include "stdafx.hpp"

#include "ar_jobs.hpp"
#include "ar_util.hpp"
#include "ar_feature.hpp"
#include "spt/features/visualizations/imgui/imgui_interface.hpp"
#include "spt/utils/interfaces.hpp"
#include "spt/utils/game_detection.hpp"

#undef clamp

#include <variant>
#include <chrono>
#include <shared_mutex>

// if enabled, writes when frames/audio are consumed to a file
#define AR_DEBUG_TIMING_INFO_FILE_NAME "ar_timing_info.txt"

#ifdef AR_DEBUG_TIMING_INFO_FILE_NAME
#include <syncstream>
#endif

// TODO go through everything and figure out what memory orderings to use
// TODO should cvar storage exec commands instead of setting cvars?

// RAII class that saves & restores the value of a cvars
class ArCvarStorage
{
	ConVar* cvar;
	std::string oldVal;
	bool doDevMsg;

public:
	ArCvarStorage(ConVar* cvar, const std::string& val, bool doDevMsg = true) : cvar(cvar), doDevMsg(doDevMsg)
	{
		if (cvar)
		{
			if (doDevMsg)
				DevMsg("SPT: setting cvar \"%s\" to \"%s\"\n", cvar->GetName(), val.c_str());
			oldVal = cvar->GetString();
			cvar->SetValue(val.c_str());
		}
	}

	ArCvarStorage(const ArCvarSetting& setting, bool doDevMsg = true)
	    : ArCvarStorage(std::holds_alternative<ConVar*>(setting.cvar)
	                        ? std::get<ConVar*>(setting.cvar)
	                        : (g_pCVar ? g_pCVar->FindVar(std::get<std::string>(setting.cvar).c_str()) : nullptr),
	                    setting.val,
	                    doDevMsg)
	{
	}

	ArCvarStorage(ArCvarStorage&) = delete;

	ArCvarStorage(ArCvarStorage&& o) : cvar(o.cvar), oldVal(std::move(o.oldVal)), doDevMsg(o.doDevMsg)
	{
		o.cvar = nullptr;
	}

	~ArCvarStorage()
	{
		if (cvar)
		{
			if (doDevMsg)
				DevMsg("SPT: setting cvar \"%s\" back to \"%s\"\n", cvar->GetName(), oldVal.c_str());
			cvar->SetValue(oldVal.c_str());
		}
	};
};

struct ArRunningJob
{
	ArRunningMovieJobStatus status;
	std::shared_ptr<ArMovieJobResult> result = std::make_shared<ArMovieJobResult>();

	struct
	{
		std::mutex clockMtx;
		std::optional<ar_elapsed_time_clock::time_point> lastMeasuredTime;
		bool lastStatePaused = false;
	} timingData;

	std::unique_ptr<ArSyncManager> mgr;
	std::vector<ArCvarStorage> cvarStorage;
	float volume = -1.f;
	bool captureAudio = false;
	bool recordAfterImGuiCallbacks = false;

	struct
	{
		/*
		* Since video and audio are handled on separate threads, I handle pausing by incrementing
		* an counter for both to specify how many frames to skip. This should minize the chances of
		* a desync if the console is spammed open/closed a bunch of times.
		*/
		std::atomic<int> nFramesToSkip;
		std::atomic<int> nAudioFramesToSkip;

		// https://en.wikipedia.org/wiki/Compare-and-swap
		// https://en.cppreference.com/w/cpp/atomic/atomic/compare_exchange.html
		template<typename T>
		static bool SubIfNonNegative(std::atomic<T>& a)
		{
			T old = a.load(std::memory_order_acquire);
			while (old > 0)
			{
				if (a.compare_exchange_weak(old,
				                            old - 1,
				                            std::memory_order_acq_rel,
				                            std::memory_order_acquire))
					return true;
			}
			return false;
		}
	} pauseCounters;

#ifdef AR_DEBUG_TIMING_INFO_FILE_NAME
	std::ofstream timingInfoFile{AR_DEBUG_TIMING_INFO_FILE_NAME};
	int64_t nAudioSamplesConsumed = 0;
#endif

	// periodically call this to update the time without pauses
	void IncrementElapsedTime(bool paused)
	{
		// the only thing that needs to be atomic is the user-accessible field, everything else can just be shoved behind a lock
		auto& td = timingData;
		std::unique_lock lk(td.clockMtx);
		auto newLastMeasuredTime = ar_elapsed_time_clock::now();
		if (!td.lastMeasuredTime.has_value())
		{
			td.lastMeasuredTime = status.startTime;
		}
		else if (!paused || !td.lastStatePaused)
		{
			status.unpausedElapsedTime.store(status.unpausedElapsedTime.load() + newLastMeasuredTime
			                                 - td.lastMeasuredTime.value());
		}
		td.lastStatePaused = paused;
		td.lastMeasuredTime = newLastMeasuredTime;
	}

	auto GetElapsedTime(bool timePauses) const
	{
		return timePauses ? ar_elapsed_time_clock::now() - status.startTime : status.unpausedElapsedTime.load();
	}
};

struct AutoRenderFeature::Impl
{
	ConVar* snd_lockpartial = nullptr;

	std::shared_mutex sharedRunningJobMtx; // starting & stopping the job is exclusive, submitting data is shared
	std::atomic<std::shared_ptr<const ArDeferredMovieJob>> deferredMovieJob;
	std::atomic<std::shared_ptr<ArRunningJob>> runningMovieJob;
	std::atomic<std::shared_ptr<ArMovieJobResult>> lastAppResult;

	void OnFrameSignal();
	void OnShaderDevicePresentPreImGuiSignal(IDirect3DDevice9* device);
	void OnShaderDevicePresentPostImGuiSignal(IDirect3DDevice9* device);
	void OnShaderDevicePresentSignal(IDirect3DDevice9* device, bool postImGui);
	void RunningJobFrame(IDirect3DDevice9* device, std::shared_ptr<ArRunningJob> runningJob, bool shouldKill);
	void ProcessDeferredJob(IDirect3DDevice9* device, std::shared_ptr<const ArDeferredMovieJob> deferredJob);
	void StoreMovieJobResult(ArRunningJob& runningJob);

	struct MovieInfoPtrs
	{
		char* moviename = nullptr;
		int* type = nullptr;

		void Init();
	} cl_movieinfo; // not stored as MovieInfo_t because the size of moviename changes between versions
};

/*
* The C++ idiomatic way to do this would be with the PImpl paradigm, but that gets weird with
* Feature::Move(). It's much easier to have a static global instead of a member field.
*/
std::unique_ptr<AutoRenderFeature::Impl> AutoRenderFeature::impl;

// TODO test on steampipe
namespace patterns
{
	PATTERNS(S_TransferStereo16,
	         "portal1-5135",
	         "53 55 56 57 E8 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ??");
	PATTERNS(CAudioDirectSound__TransferSamples, "portal1-5135", "83 EC 08 56 8B F1 80 7E ?? 00 57");
	PATTERNS(CVideoMode_Common__WriteMovieFrame,
	         "portal1-5135",
	         "80 3D ?? ?? ?? ?? 00 55 8B 6C 24 ?? 8B 85 ?? ?? ?? ??");
} // namespace patterns

bool AutoRenderFeature::ShouldLoadFeature()
{
	return !!g_pCVar && !!interfaces::_engine_client;
}

void AutoRenderFeature::Impl::MovieInfoPtrs::Init()
{
	// find cl_movieinfo struct
	if (!g_pCVar)
		return;
	const ConCommand* endmovieCmd = g_pCVar->FindCommand("endmovie");
	if (!endmovieCmd)
		return;
	const byte* cbFunc = *(byte**)((byte*)endmovieCmd + sizeof(ConCommandBase));
	if (!cbFunc)
		return;
	if (cbFunc[0] != 0x80 || cbFunc[1] != 0x3d || cbFunc[6] != 0x00) // cmp byte ptr [cl_movieinfo.moviename], 0
		return;
	moviename = *(char**)(cbFunc + 2);

	int typeOff = utils::GetBuildNumber() <= 5135 ? 260 : 264;
	type = (int*)(moviename + typeOff);
}

void AutoRenderFeature::InitHooks()
{
	HOOK_FUNCTION(engine, S_TransferStereo16);
	HOOK_FUNCTION(engine, CAudioDirectSound__TransferSamples);
	HOOK_FUNCTION(engine, CVideoMode_Common__WriteMovieFrame);

	// setup before LoadFeature()
	impl = std::make_unique<Impl>();
	impl->snd_lockpartial = g_pCVar->FindVar("snd_lockpartial");
	impl->cl_movieinfo.Init();
}

void AutoRenderFeature::LoadFeature()
{
	if (!Works())
		return;

	// fill in default placeholders

	// TODO test with portal in a non-ascii folder
	std::filesystem::path modDir = interfaces::_engine_client->GetGameDirectory();
	modDir = std::filesystem::canonical(modDir);
	ArGlobalPlaceholders::MOD_DIR.SetValue(modDir.string());
	std::error_code ec;
	std::filesystem::path workingDir = std::filesystem::current_path(ec);
	ArGlobalPlaceholders::GAME_WORKING_DIR.SetValue((ec ? modDir : workingDir).string());
	ArGlobalPlaceholders::RENDER_WORKING_DIR.SetValue((workingDir / "spt_autorender").string());
	ArGlobalPlaceholders::EXE_PATH.SetValue("C:\\YOUR_MOM\\ffmpeg.exe");
	ArGlobalPlaceholders::FRAMERATE.SetValue("60");

	ArGlobalPlaceholders::UUID.Regenerate();
	ArGlobalPlaceholders::PIPE_NAME.Regenerate();

	FrameSignal.Connect(impl.get(), &AutoRenderFeature::Impl::OnFrameSignal);
	ShaderDevicePresentPreImGuiSignal.Connect(impl.get(),
	                                          &AutoRenderFeature::Impl::OnShaderDevicePresentPreImGuiSignal);
	ShaderDevicePresentPostImGuiSignal.Connect(impl.get(),
	                                           &AutoRenderFeature::Impl::OnShaderDevicePresentPostImGuiSignal);

	SptImGuiGroup::QoL_AutoRender.RegisterUserCallback(ImGuiTabCallback);
}

void AutoRenderFeature::UnloadFeature()
{
	impl.reset();
}

bool AutoRenderFeature::Works()
{
	return ShaderDevicePresentPreImGuiSignal.Works && ShaderDevicePresentPostImGuiSignal.Works && FrameSignal.Works;
}

bool AutoRenderFeature::SupportsAudioCapture()
{
	return impl->snd_lockpartial && ORIG_CAudioDirectSound__TransferSamples && ORIG_S_TransferStereo16
	       && impl->cl_movieinfo.moviename && impl->cl_movieinfo.type;
}

std::vector<ArCvarSetting> AutoRenderFeature::CreateDefaultCvarSettings(float hostFrameRateVal)
{
	std::vector<ArCvarSetting> ret;
	ret.emplace_back("sv_cheats", "1");
	ret.emplace_back("volume", "0");
	ret.emplace_back("host_framerate", std::to_string(hostFrameRateVal).c_str()); // TODO substitute
	ret.emplace_back("gl_clear", "1");                                            // TODO make this optional
	// ret.emplace_back("snd_noextraupdate", "1");                                   // TODO do I need this?
	// ret.emplace_back("snd_mixahead", "0");
	ret.emplace_back("snd_surround", "2"); // TODO do I need to pair this with snd_lockpartial?
	ret.emplace_back("spt_focus_nosleep", "1");
	ret.emplace_back("spt_disable_tone_map_reset", "1");
	ret.emplace_back("spt_override_tpose", "17");
	return ret;
}

void AutoRenderFeature::QueueMovieJob(std::unique_ptr<const ArDeferredMovieJob> deferred)
{
	impl->deferredMovieJob.store(std::move(deferred));
}

std::shared_ptr<const ArRunningMovieJobStatus> AutoRenderFeature::GetRunningJobStatus() const
{
	auto runningJob = impl->runningMovieJob.load();
	// aliasing ctor of std::shared_ptr
	return runningJob ? std::shared_ptr<const ArRunningMovieJobStatus>(runningJob, &runningJob->status) : nullptr;
}

std::shared_ptr<const ArMovieJobResult> AutoRenderFeature::GetLastMovieJobResult() const
{
	return impl->lastAppResult.load();
}

bool AutoRenderFeature::PauseMovieJob(bool pauseState)
{
	auto runningJob = impl->runningMovieJob.load();
	if (runningJob)
		runningJob->status.userPaused.store(pauseState);
	return !!runningJob;
}

bool AutoRenderFeature::StopMovieJob()
{
	std::lock_guard lk(impl->sharedRunningJobMtx);

	bool killed = false;
	auto runningJob = impl->runningMovieJob.exchange(nullptr, std::memory_order_acquire);

	if (runningJob)
	{
		// Finish() may take a while, so record how long that takes
		runningJob->IncrementElapsedTime(false);
		runningJob->mgr->Finish(runningJob->result->stat);
		runningJob->IncrementElapsedTime(false);

		impl->StoreMovieJobResult(*runningJob);
		if (runningJob->captureAudio)
			impl->cl_movieinfo.moviename[0] = '\0'; // TODO make this RAII?
		killed = true;
	}

	return killed;
}

// TODO do I still need this hook? if not, then I don't need snd_lockpartial either
IMPL_HOOK_THISCALL(AutoRenderFeature, void, CAudioDirectSound__TransferSamples, void*, int end)
{
	auto runningMovieJob = impl->runningMovieJob.load();

	if (!!runningMovieJob && runningMovieJob->captureAudio)
	{
		// force the code in the game to call S_TransferStereo16

		struct CAudioDeviceBase
		{
			void* vt;
			bool m_bSurround;
		};

		CAudioDeviceBase* audioDev = (CAudioDeviceBase*)thisptr;

		ArCvarStorage lockPartial(impl->snd_lockpartial, "0", false);
		bool oldSurroundVal = audioDev->m_bSurround;
		audioDev->m_bSurround = false;
		ORIG_CAudioDirectSound__TransferSamples(thisptr, end);
		audioDev->m_bSurround = oldSurroundVal;
	}
	else
	{
		ORIG_CAudioDirectSound__TransferSamples(thisptr, end);
	}
}

IMPL_HOOK_CDECL(AutoRenderFeature,
                void,
                S_TransferStereo16,
                void* pOutput,
                const void* pfront,
                int lpaintedtime,
                int endtime)
{
	ORIG_S_TransferStereo16(pOutput, pfront, lpaintedtime, endtime);

#if 0

	int nPairs = endtime - lpaintedtime;
	if (nPairs <= 0)
		return;

	std::shared_lock lk(impl->sharedRunningJobMtx);

	auto runningJob = impl->runningMovieJob.load();
	if (!runningJob)
		return;

	bool paused = runningJob->pauseCounters.SubIfNonNegative(runningJob->pauseCounters.nAudioFramesToSkip);

#ifdef AR_DEBUG_TIMING_INFO_FILE_NAME
	std::osyncstream(runningJob->timingInfoFile)
	    << std::format("[{}]: {} samples (total={}) (timestamp={:.3f}), paused={}\n",
	                   __FUNCTION__,
	                   nPairs,
	                   runningJob->nAudioSamplesConsumed,
	                   runningJob->nAudioSamplesConsumed / 44100.f,
	                   paused);
#endif

	if (paused)
		return;

	// error will get picked up by the render thread
	ser::StatusTracker stat;
	short* outBuf = runningJob->mgr->LockSoundBuf(nPairs, stat);
	if (!outBuf || !stat.Ok())
		return;

	float vol = runningJob->volume;
	for (int i = 0; i < nPairs * 2; i++)
	{
		float sample = ((int*)pfront)[i] * vol;
		outBuf[i] = static_cast<short>(std::clamp(sample, -32768.f, 32767.f));
	}

	runningJob->mgr->UnlockSoundBuf(stat);

#ifdef AR_DEBUG_TIMING_INFO_FILE_NAME
	runningJob->nAudioSamplesConsumed += nPairs;
#endif

#else

	// NEW

	std::shared_lock lk(impl->sharedRunningJobMtx);

	auto runningJob = impl->runningMovieJob.load(std::memory_order_acquire);
	if (!runningJob)
		return;

	// TODO handle paused state

	int bufferSizeBytes = 0x4000 * 2;
	int deviceSampleCount = bufferSizeBytes / 2;
	int* snd_p = (int*)pfront;

	int samplePairCount = deviceSampleCount / 2;
	int sampleMask = samplePairCount - 1;
	float vol = runningJob->volume;

	while (lpaintedtime < endtime)
	{
		int lpos = lpaintedtime & sampleMask;
		int snd_linear_count = std::min(samplePairCount - lpos, endtime - lpaintedtime);

#ifdef AR_DEBUG_TIMING_INFO_FILE_NAME
		std::osyncstream(runningJob->timingInfoFile)
		    << std::format("[{}]: {} samples (total={}) (timestamp={:.3f}), paused={}\n",
		                   __FUNCTION__,
		                   snd_linear_count,
		                   runningJob->nAudioSamplesConsumed,
		                   runningJob->nAudioSamplesConsumed / 44100.f,
		                   false);
		runningJob->nAudioSamplesConsumed += snd_linear_count;
#endif

		{
			ser::StatusTracker stat;
			short* outBuf = runningJob->mgr->LockSoundBuf(snd_linear_count, stat);
			if (!outBuf || !stat.Ok())
				return;
			for (int i = 0; i < snd_linear_count * 2; i++)
			{
				float sample = ((int*)snd_p)[i] * vol;
				outBuf[i] = static_cast<short>(std::clamp(sample, -32768.f, 32767.f));
			}
			runningJob->mgr->UnlockSoundBuf(stat);
		}

		snd_p += snd_linear_count * 2;
		lpaintedtime += snd_linear_count;
	}

#endif
}

IMPL_HOOK_THISCALL(AutoRenderFeature, void, CVideoMode_Common__WriteMovieFrame, void*, void* movieInfo)
{
	if (!impl->runningMovieJob.load(std::memory_order_acquire))
		ORIG_CVideoMode_Common__WriteMovieFrame(thisptr, movieInfo);
}

void AutoRenderFeature::Impl::OnFrameSignal()
{
	ArGlobalPlaceholders::DATE_TIME.Update();

	// update width/height placeholders
	int width, height;
	interfaces::_engine_client->GetScreenSize(width, height);
	ArGlobalPlaceholders::VID_WIDTH.SetValue(std::to_string(width));
	ArGlobalPlaceholders::VID_HEIGHT.SetValue(std::to_string(height));

	// handle pausing

	auto runningJob = runningMovieJob.load();
	if (runningJob)
	{
		bool consoleOpen = interfaces::_engine_client->Con_IsVisible();
		bool paused =
		    runningJob->status.userPaused || (runningJob->status.recordWhenConsoleIsOpen != consoleOpen);
		if (paused)
		{
			runningJob->pauseCounters.nFramesToSkip.fetch_add(1, std::memory_order_release);
			runningJob->pauseCounters.nAudioFramesToSkip.fetch_add(1, std::memory_order_release);
		}
#ifdef AR_DEBUG_TIMING_INFO_FILE_NAME
		std::osyncstream(runningJob->timingInfoFile)
		    << std::format("[{}]: paused={} (framesToPause={}, audioFramesToPause={})\n",
		                   __FUNCTION__,
		                   paused,
		                   runningJob->pauseCounters.nFramesToSkip.load(std::memory_order_acquire),
		                   runningJob->pauseCounters.nAudioFramesToSkip.load(std::memory_order_acquire));
#endif
	}
}

void AutoRenderFeature::Impl::OnShaderDevicePresentPreImGuiSignal(IDirect3DDevice9* device)
{
	OnShaderDevicePresentSignal(device, false);
}

void AutoRenderFeature::Impl::OnShaderDevicePresentPostImGuiSignal(IDirect3DDevice9* device)
{
	OnShaderDevicePresentSignal(device, true);
}

void AutoRenderFeature::Impl::OnShaderDevicePresentSignal(IDirect3DDevice9* device, bool postImGui)
{
	auto deferredJob = deferredMovieJob.exchange(nullptr);
	auto runningJob = runningMovieJob.load();
	if (runningJob && (runningJob->recordAfterImGuiCallbacks == postImGui))
		RunningJobFrame(device, std::move(runningJob), !!deferredJob);
	if (deferredJob)
	{
		ProcessDeferredJob(device, std::move(deferredJob));

		/*
		* In my mind, ideally we would submit a frame as soon as we turn a deferred job into a
		* running one, but I think not doing so is really the way to go. If we submit now:
		* - the audio will be delayed by one frame
		* - we would have to handle calling RunningJobFrame in the post ImGui callback but 
		*   the user not wanting ImGui to show up in their video
		*/
	}
}

void AutoRenderFeature::Impl::RunningJobFrame(IDirect3DDevice9* device,
                                              std::shared_ptr<ArRunningJob> runningJob,
                                              bool shouldKill)
{
	if (!runningJob)
		return;

	{
		std::shared_lock lk(sharedRunningJobMtx);

		/*
		* Kill the current job if:
		* - there's a new job to queue
		* - the job failed
		* - the job had an error while consuming a new frame
		* - the job consumed enough frames
		*/
		bool paused = runningJob->pauseCounters.SubIfNonNegative(runningJob->pauseCounters.nFramesToSkip);

#ifdef AR_DEBUG_TIMING_INFO_FILE_NAME
		auto _nConsumed = runningJob->status.nFramesConsumed.load(std::memory_order_acquire);
		std::osyncstream(runningJob->timingInfoFile)
		    << std::format("[{}]: frame {} (timestamp={:.3f}), paused={}\n",
		                   __FUNCTION__,
		                   _nConsumed,
		                   _nConsumed / runningJob->status.outputFramerate,
		                   paused);
#endif

		if (!paused)
		{
			if (!shouldKill)
			{
				runningJob->mgr->OnDevicePresent(device, runningJob->result->stat);
				shouldKill |= !runningJob->result->stat.Ok();
			}
			if (!shouldKill)
			{
				auto nConsumed =
				    runningJob->status.nFramesConsumed.fetch_add(1, std::memory_order_release) + 1;
				auto maxConsumeFrames = runningJob->status.maxConsumeFrames;
				if (maxConsumeFrames.has_value())
				{
					Assert(maxConsumeFrames.value() >= 1);
					shouldKill |= nConsumed >= maxConsumeFrames.value();
				}
			}
		}
		runningJob->IncrementElapsedTime(paused);
	}

	if (shouldKill)
		spt_auto_render_feat.StopMovieJob();
}

void AutoRenderFeature::Impl::ProcessDeferredJob(IDirect3DDevice9* device,
                                                 std::shared_ptr<const ArDeferredMovieJob> deferredJob)
{
	if (!deferredJob)
		return;

	// mY bOdy Is a mAChInE tHaT tUrnS deFeRreD jObS iNtO rUnNinG jObS

	auto startTime = ar_elapsed_time_clock::now();

	// init the running job object

	std::shared_ptr<ArRunningJob> newRunningJob = std::make_shared<ArRunningJob>();
	newRunningJob->status.startTime = startTime;
	newRunningJob->status.recordWhenConsoleIsOpen = deferredJob->recordWhenConsoleIsOpen;
	newRunningJob->status.maxConsumeFrames = deferredJob->maxConsumeFrames;
	newRunningJob->status.outputFramerate = deferredJob->ffmpegArgs.framerate;
	newRunningJob->volume = deferredJob->volume;
	newRunningJob->captureAudio =
	    deferredJob->ffmpegArgs.captureAudio && spt_auto_render_feat.SupportsAudioCapture();
	newRunningJob->recordAfterImGuiCallbacks = deferredJob->recordAfterImGuiCallbacks;

	// create consumer (init pipe, ffmpeg process, nutlib, etc.)

	auto& stat = newRunningJob->result->stat;

	std::unique_ptr<ArLockableSurfaceConsumer> consumer =
	    std::make_unique<ArFfmpegWriter>(deferredJob->ffmpegArgs, newRunningJob->result->returnCode, stat);

	if (stat.Ok())
	{
		// init consumer manager (create render targets, start up threads, etc.)
		switch (deferredJob->syncMode)
		{
		case AR_SYNC_FULL:
			newRunningJob->mgr = std::make_unique<ArSynchronousConsumerManager>(device,
			                                                                    D3DFMT_A8R8G8B8,
			                                                                    std::move(consumer),
			                                                                    stat);
			break;
		case AR_SYNC_THREADED:
			newRunningJob->mgr =
			    std::make_unique<ArThreadedConsumerManager>(device,
			                                                D3DFMT_A8R8G8B8,
			                                                deferredJob->nFramesInFlight.value(),
			                                                std::move(consumer),
			                                                stat);
			break;
		default:
			Assert(0);
			stat.Err("unknown sync mode");
		}
	}

	if (!stat.Ok())
	{
		StoreMovieJobResult(*newRunningJob);
		return;
	}

	// kill old job and reset cvars

	spt_auto_render_feat.StopMovieJob();

	// a small race condition here between StopMovieJob & lock(sharedRunningJobMtx), but whatever

	{
		// load new cvars, set moviename and swap out the running job object
		std::lock_guard lk(sharedRunningJobMtx);
		std::ranges::copy(deferredJob->cvars, std::back_inserter(newRunningJob->cvarStorage));
		if (newRunningJob->captureAudio)
		{
			// TODO document why the hell we need to do this
			*impl->cl_movieinfo.type = 0;
			strcpy(impl->cl_movieinfo.moviename, "spt_autorender");
		}
		newRunningJob->IncrementElapsedTime(false);
		runningMovieJob.store(newRunningJob);
	}

	ArGlobalPlaceholders::UUID.Regenerate();
	ArGlobalPlaceholders::PIPE_NAME.Regenerate();
}

void AutoRenderFeature::Impl::StoreMovieJobResult(ArRunningJob& runningJob)
{
	// fill out runningJob->result and save it
	auto& result = runningJob.result;
	result->elapsedTime = runningJob.GetElapsedTime(true);
	result->unpausedElapsedTime = runningJob.GetElapsedTime(false);
	result->nFramesConsumed = runningJob.status.nFramesConsumed.load(std::memory_order_acquire);
	impl->lastAppResult.store(std::move(result), std::memory_order_release);
}
