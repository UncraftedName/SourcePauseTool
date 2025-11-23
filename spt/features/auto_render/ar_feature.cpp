#include "stdafx.hpp"

#include "ar_jobs.hpp"
#include "ar_util.hpp"
#include "ar_interface.hpp"
#include "ar_placeholders.hpp"
#include "spt/features/visualizations/imgui/imgui_interface.hpp"
#include "spt/utils/interfaces.hpp"
#include "spt/utils/game_detection.hpp"
#include "spt/features/demo.hpp"

#undef clamp

#include <variant>
#include <chrono>
#include <shared_mutex>
#include <syncstream>

#define AR_SAMPLERATE 44100

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
	bool initializedFromMultiDemo = false;

	// debug file for dumping timestamps
	std::ofstream timingInfoFile;

	struct
	{
		/*
		* Since video and audio are handled on separate threads, I handle pausing by incrementing
		* an counter for both to specify how many frames to skip. This should minimize the chances
		* of a desync if the console is spammed open/closed a bunch of times. For audio, keeping
		* track of the number of samples to decrement instead of frames should be slightly more
		* accurate and should help keep the audio and video in sync.
		*/
		std::atomic<int> nFramesToSkip;
		std::atomic<int> nAudioSamplesToSkip;

		/*
		* pauseCounter - how much more we should pause for
		* timeAdvanceAttempt - how much we are advancing time (e.g. number of frames or samples)
		* 
		* Try to subtract the counter by timeAdvanceAttempt and return the amount we should
		* actually advance time by. Result after subtraction:
		* - (counter > 0)  -> return 0, caller does no work
		* - (counter <= 0) -> return timeAdvanceAttempt - oldCounterVal
		* 
		* https://en.wikipedia.org/wiki/Compare-and-swap
		* https://en.cppreference.com/w/cpp/atomic/atomic/compare_exchange.html
		*/
		static int CalcTimeAdvance(std::atomic<int>& pauseCounter, int timeAdvanceAttempt)
		{
			Assert(timeAdvanceAttempt >= 0);
			int oldVal = pauseCounter.load(std::memory_order_acquire);
			while (oldVal > 0)
			{
				int actualTimeAdvance = std::max(0, timeAdvanceAttempt - oldVal);
				int newVal = std::max(0, oldVal - timeAdvanceAttempt);
				if (pauseCounter.compare_exchange_weak(oldVal,
				                                       newVal,
				                                       std::memory_order_acq_rel,
				                                       std::memory_order_acquire))
				{
					return actualTimeAdvance;
				}
			}
			return timeAdvanceAttempt;
		}

		void FrameCheckPause(const ArRunningMovieJobStatus& status)
		{
			bool consoleOpen = interfaces::_engine_client->Con_IsVisible();
			bool paused = status.recordWhenConsoleIsOpen != consoleOpen;
			if (paused)
			{
				nFramesToSkip.fetch_add(1, std::memory_order_release);
				nAudioSamplesToSkip.fetch_add((int)(AR_SAMPLERATE / status.framerate),
				                              std::memory_order_release);
			}
		}
	} pauseCounters;

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

struct ArRunningMultiDemoJob
{
	ArRunningMultiDemoJobStatus status;
	std::shared_ptr<ArDeferredMovieJob> templateDeferredJob;
	/*
	* Make the datetime placeholder the same for each video in a multi-demo job. This allows you to
	* easily see which videos come from the same job and enables windows to sort the video files in
	* the same order as the demo files they come from.
	*/
	ArPhDatetime phDatetimeReplacement = ArGlobalPlaceholders::DATE_TIME.CopyByKey<ArPhDatetime>();
};

class AutoRenderFeature : public FeatureWrapper<AutoRenderFeature>
{
protected:
	virtual bool ShouldLoadFeature() override;
	virtual void InitHooks() override;
	virtual void LoadFeature() override;
	virtual void UnloadFeature() override;

public:
	// static inline hell to work with FeatureWrapper::Move

	// starting & stopping an ArRunningJob is exclusive, submitting video/audio data is shared
	// TODO is this necessary?
	static inline std::shared_mutex sharedRunningJobMtx;

	static inline std::atomic<std::shared_ptr<ArRunningMultiDemoJob>> runningMultiDemoJob;
	static inline std::atomic<std::shared_ptr<ArDeferredMovieJob>> deferredMovieJob;
	static inline std::atomic<std::shared_ptr<ArRunningJob>> runningMovieJob;
	static inline std::atomic<std::shared_ptr<ArMovieJobResult>> lastMovieJobResult;

	void OnFrameSignal();
	static void OnShaderDevicePresentPreImGuiSignal(IDirect3DDevice9* device);
	static void OnShaderDevicePresentPostImGuiSignal(IDirect3DDevice9* device);
	void OnShaderDevicePresentSignal(IDirect3DDevice9* device, bool postImGui);
	void RunningJobFrame(IDirect3DDevice9* device, std::shared_ptr<ArRunningJob> runningJob, bool shouldKill);
	void ProcessDeferredJob(IDirect3DDevice9* device,
	                        std::shared_ptr<ArDeferredMovieJob> deferredJob,
	                        std::shared_ptr<ArRunningMultiDemoJob> fromMultiDemoJob);
	void StoreMovieJobResult(ArRunningJob& runningJob);
	/*
	* A separate impl from the interface that doesn't lock since locking a shared mutex recursively
	* is bad. Make sure to lock sharedRunningJobMtx exclusively before calling this.
	*/
	bool StopMovieJobNoLock();

	struct MovieInfoPtrs
	{
		char* moviename = nullptr;
		int* type = nullptr;

		void Init();
	} cl_movieinfo; // not stored as MovieInfo_t because the size of moviename changes between versions

	DECL_STATIC_HOOK_THISCALL(void, CAudioDirectSound__TransferSamples, void*, int end);

	DECL_STATIC_HOOK_CDECL(void,
	                       S_TransferStereo16,
	                       void* pOutput,
	                       const void* pfront,
	                       int lpaintedtime,
	                       int endtime);

	DECL_STATIC_HOOK_THISCALL(void, CVideoMode_Common__WriteMovieFrame, void*, void* info);

} static spt_auto_render_feat;

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

void AutoRenderFeature::MovieInfoPtrs::Init()
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
	cl_movieinfo.Init();
}

// ImGui callbacks are not member functions so I can test decoupling them from internal feature state
extern void ArImGuiTabCallbackEntry();
extern void ArImGuiFileDialogWindowEntry();

void AutoRenderFeature::LoadFeature()
{
	if (!SptAutoRender::Works())
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

	ArGlobalPlaceholders::DEMO_SEQ.SetValue("_0");
	ArGlobalPlaceholders::DEMO_NAME.SetValue("_nodemo");
	ArGlobalPlaceholders::EXE_PATH.FindFfmpeg();

	FrameSignal.Connect(this, &AutoRenderFeature::OnFrameSignal);
	ShaderDevicePresentPreImGuiSignal.Connect(&AutoRenderFeature::OnShaderDevicePresentPreImGuiSignal);
	ShaderDevicePresentPostImGuiSignal.Connect(&AutoRenderFeature::OnShaderDevicePresentPostImGuiSignal);

	SptImGuiGroup::QoL_AutoRender.RegisterUserCallback(ArImGuiTabCallbackEntry);
	SptImGui::RegisterWindowCallback(ArImGuiFileDialogWindowEntry);
}

void AutoRenderFeature::UnloadFeature()
{
	runningMultiDemoJob.store(nullptr, std::memory_order_release);
	deferredMovieJob.store(nullptr, std::memory_order_release);
	runningMovieJob.store(nullptr, std::memory_order_release);
	lastMovieJobResult.store(nullptr, std::memory_order_release);
	cl_movieinfo.moviename = nullptr;
	cl_movieinfo.type = nullptr;
}

bool SptAutoRender::Works()
{
	return ShaderDevicePresentPreImGuiSignal.Works && ShaderDevicePresentPostImGuiSignal.Works
	       && spt_auto_render_feat.ORIG_CVideoMode_Common__WriteMovieFrame && FrameSignal.Works;
}

bool SptAutoRender::MultiDemoJobWorks()
{
	return Works() && !!spt_demostuff.pDemoplayer;
}

bool SptAutoRender::SupportsAudioCapture()
{
	return spt_auto_render_feat.ORIG_CAudioDirectSound__TransferSamples
	       && spt_auto_render_feat.ORIG_S_TransferStereo16 && spt_auto_render_feat.cl_movieinfo.moviename
	       && spt_auto_render_feat.cl_movieinfo.type;
}

bool SptAutoRender::QueueSingleMovieJob(std::unique_ptr<ArDeferredMovieJob> deferred)
{
	if (!Works())
		return false;
	if (spt_auto_render_feat.runningMultiDemoJob.load(std::memory_order_acquire))
	{
		return false;
	}
	else
	{
		spt_auto_render_feat.deferredMovieJob.store(std::move(deferred), std::memory_order_release);
		return true;
	}
}

bool SptAutoRender::QueueMultiDemoJob(std::unique_ptr<ArDeferredMovieJob> templateDeferredJob,
                                      std::vector<std::filesystem::path> demoFilePaths)
{
	if (!MultiDemoJobWorks())
		return false;
	if (demoFilePaths.empty())
		return true;

	if (spt_auto_render_feat.deferredMovieJob.load(std::memory_order_acquire))
		return false;

	std::shared_ptr<ArRunningMultiDemoJob> newMultiDemoJob = std::make_shared<ArRunningMultiDemoJob>();
	newMultiDemoJob->phDatetimeReplacement.Update(); // only time this is ever set
	newMultiDemoJob->status.startTime = ar_elapsed_time_clock::now();
	newMultiDemoJob->status.demoFilePaths = std::move(demoFilePaths);
	newMultiDemoJob->templateDeferredJob = std::move(templateDeferredJob);

	std::shared_ptr<ArRunningMultiDemoJob> multiDemoJobExpected = nullptr;
	bool set = spt_auto_render_feat.runningMultiDemoJob.compare_exchange_strong(multiDemoJobExpected,
	                                                                            newMultiDemoJob,
	                                                                            std::memory_order_release,
	                                                                            std::memory_order_relaxed);
	if (set)
		SptAutoRender::StopMovieJob();
	return set;
}

std::shared_ptr<const ArRunningMovieJobStatus> SptAutoRender::GetRunningMovieJobStatus()
{
	auto runningJob = spt_auto_render_feat.runningMovieJob.load();
	return runningJob ? std::shared_ptr<const ArRunningMovieJobStatus>(runningJob, &runningJob->status) : nullptr;
}

std::shared_ptr<const ArRunningMultiDemoJobStatus> SptAutoRender::GetRunningMultiDemoJobStatus()
{
	auto multiDemoJob = spt_auto_render_feat.runningMultiDemoJob.load(std::memory_order_acquire);
	return multiDemoJob ? std::shared_ptr<const ArRunningMultiDemoJobStatus>(multiDemoJob, &multiDemoJob->status)
	                    : nullptr;
}

std::shared_ptr<const ArMovieJobResult> SptAutoRender::GetLastMovieJobResult()
{
	return spt_auto_render_feat.lastMovieJobResult.load();
}

bool SptAutoRender::StopMovieJob()
{
	std::lock_guard lk(spt_auto_render_feat.sharedRunningJobMtx);
	return spt_auto_render_feat.StopMovieJobNoLock();
}

bool SptAutoRender::StopMultiDemoJob()
{
	bool hadJob = !!spt_auto_render_feat.runningMultiDemoJob.exchange(nullptr, std::memory_order_acq_rel);
	if (hadJob)
	{
		// TODO reset demo placeholders here and anywhere else where we kill the multi demo job
		StopMovieJob();
		interfaces::_engine_client->ClientCmd("stopdemo");
		return true;
	}
	return false;
}

bool AutoRenderFeature::StopMovieJobNoLock()
{
	bool killed = false;
	auto runningJob = runningMovieJob.exchange(nullptr, std::memory_order_acquire);

	if (runningJob)
	{
		// Finish() may take a while, so record how long that takes
		runningJob->IncrementElapsedTime(false);
		runningJob->mgr->Finish(runningJob->result->stat);
		runningJob->IncrementElapsedTime(false);

		StoreMovieJobResult(*runningJob);
		if (runningJob->captureAudio)
			cl_movieinfo.moviename[0] = '\0'; // TODO make this RAII?
		killed = true;
	}

	return killed;
}

// TODO do I still need this hook? if not, then I don't need snd_lockpartial either
IMPL_HOOK_THISCALL(AutoRenderFeature, void, CAudioDirectSound__TransferSamples, void*, int end)
{
	auto running = runningMovieJob.load(std::memory_order_acquire);

	if (!!running && running->captureAudio)
	{
		// force the code in the game to call S_TransferStereo16

		struct CAudioDeviceBase
		{
			void* vt;
			bool m_bSurround;
		};

		CAudioDeviceBase* audioDev = (CAudioDeviceBase*)thisptr;
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

// TODO track audio and video sample count and error out if they desync too much

IMPL_HOOK_CDECL(AutoRenderFeature,
                void,
                S_TransferStereo16,
                void* pOutput,
                const void* pfront,
                int lpaintedtime,
                int endtime)
{
	ORIG_S_TransferStereo16(pOutput, pfront, lpaintedtime, endtime);

	int nPairsFromGame = endtime - lpaintedtime;
	if (nPairsFromGame <= 0)
		return;

	std::shared_lock lk(sharedRunningJobMtx);

	auto runningJob = runningMovieJob.load();
	if (!runningJob || !runningJob->captureAudio)
		return;

	// the actual number of pairs we're processing
	int nPairs =
	    runningJob->pauseCounters.CalcTimeAdvance(runningJob->pauseCounters.nAudioSamplesToSkip, nPairsFromGame);

	if (runningJob->timingInfoFile.is_open())
	{
		size_t nSamplesConsumed = runningJob->status.nAudioSamplesConsumed.load(std::memory_order_acquire);
		std::osyncstream(runningJob->timingInfoFile) << std::format(
		    "[{}]: {} samples consumed ({} from game this call, timestamp={:.3f}), processing {} samples\n",
		    __FUNCTION__,
		    nSamplesConsumed,
		    nPairsFromGame,
		    nSamplesConsumed / (float)AR_SAMPLERATE,
		    nPairs);
	}

	if (nPairs <= 0)
		return;

	// skip over samples to emulate pausing
	pfront = (int*)pfront + (nPairsFromGame - nPairs) * 2;

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

	runningJob->status.nAudioSamplesConsumed.fetch_add(nPairs, std::memory_order_release);
}

IMPL_HOOK_THISCALL(AutoRenderFeature, void, CVideoMode_Common__WriteMovieFrame, void*, void* movieInfo)
{
	if (!runningMovieJob.load(std::memory_order_acquire))
		ORIG_CVideoMode_Common__WriteMovieFrame(thisptr, movieInfo);
}

void AutoRenderFeature::OnFrameSignal()
{
	ArGlobalPlaceholders::DATE_TIME.Update();

	// update width/height placeholders
	int width, height;
	interfaces::_engine_client->GetScreenSize(width, height);
	ArGlobalPlaceholders::VID_WIDTH.SetValue(std::to_string(width));
	ArGlobalPlaceholders::VID_HEIGHT.SetValue(std::to_string(height));

	auto runningJob = runningMovieJob.load(std::memory_order_acquire);
	if (runningJob)
	{
		// multi demo job and demo is done -> move on to next demo
		if (runningJob->status.nFramesConsumed.load(std::memory_order_acquire) > 0
		    && runningJob->initializedFromMultiDemo)
		{
			if (!spt_demostuff.Demo_IsPlayingBack())
				SptAutoRender::StopMovieJob();
		}
	}

	// TODO somehow check if the demo failed to load and move on to the next one
}

void AutoRenderFeature::OnShaderDevicePresentPreImGuiSignal(IDirect3DDevice9* device)
{
	spt_auto_render_feat.OnShaderDevicePresentSignal(device, false);
}

void AutoRenderFeature::OnShaderDevicePresentPostImGuiSignal(IDirect3DDevice9* device)
{
	spt_auto_render_feat.OnShaderDevicePresentSignal(device, true);
}

void AutoRenderFeature::OnShaderDevicePresentSignal(IDirect3DDevice9* device, bool postImGui)
{
	auto deferredJob = deferredMovieJob.exchange(nullptr);
	auto runningJob = runningMovieJob.load();
	if (runningJob && (runningJob->recordAfterImGuiCallbacks == postImGui))
		RunningJobFrame(device, runningJob, !!deferredJob);

	if (deferredJob)
	{
		// init deferred job
		ProcessDeferredJob(device, std::move(deferredJob), nullptr);
	}
	else if (!runningJob)
	{
		// init multi-demo job
		auto multiDemoJob = runningMultiDemoJob.load(std::memory_order_acquire);
		if (multiDemoJob)
		{
			if (multiDemoJob->status.nextDemoIdx.load(std::memory_order_acquire)
			    >= multiDemoJob->status.demoFilePaths.size())
			{
				SptAutoRender::StopMultiDemoJob();
			}
			else
			{
				// TODO add option for what happens if the previous job fails (continue or exit)
				// TODO force console closed somehow...
				ProcessDeferredJob(device, multiDemoJob->templateDeferredJob, multiDemoJob);
			}
		}
	}
}

void AutoRenderFeature::RunningJobFrame(IDirect3DDevice9* device,
                                        std::shared_ptr<ArRunningJob> runningJob,
                                        bool shouldKill)
{
	{
		std::shared_lock lk(sharedRunningJobMtx);

		if (!runningJob)
			return;

		runningJob->pauseCounters.FrameCheckPause(runningJob->status);
		bool paused =
		    runningJob->pauseCounters.CalcTimeAdvance(runningJob->pauseCounters.nFramesToSkip, 1) <= 0;

		/*
		* Kill the current job if:
		* - there's a new job to queue (shouldKill is already true)
		* - the job failed
		* - the job had an error while consuming a new frame
		* - the job consumed enough frames
		* - the audio and video are too far out of sync (this causes nutlib to eat all of our memory)
		*/

		if (!shouldKill && runningJob->captureAudio)
		{
			// check audio/video desync
			double vidTimestamp = runningJob->status.nFramesConsumed.load(std::memory_order_acquire)
			                      / (double)runningJob->status.framerate;
			double audioTimestamp = runningJob->status.nAudioSamplesConsumed.load(std::memory_order_acquire)
			                        / (double)AR_SAMPLERATE;
			double diff = std::abs(vidTimestamp - audioTimestamp);
			double nFramesDesynced = diff * runningJob->status.framerate;
			if (nFramesDesynced > 15)
			{
				runningJob->result->stat.Err(
				    std::format("SPT: The audio and video desynced too much "
				                "(vid ts={:.3f}s, audio ts={:.3f}s, approx {} frames desync), "
				                "aborting to prevent OOM error",
				                vidTimestamp,
				                audioTimestamp,
				                (int)nFramesDesynced));
				shouldKill = true;
			}

			if (runningJob->timingInfoFile.is_open())
			{
				std::osyncstream(runningJob->timingInfoFile)
				    << std::format("[{}]: Timestamp desync is {:.3f} (~{:.1f} frames)\n",
				                   __FUNCTION__,
				                   vidTimestamp - audioTimestamp,
				                   nFramesDesynced);
			}
		}

		if (runningJob->timingInfoFile.is_open())
		{
			auto _nConsumed = runningJob->status.nFramesConsumed.load(std::memory_order_acquire);
			std::osyncstream(runningJob->timingInfoFile)
			    << std::format("[{}]: {} frames consumed (timestamp={:.3f}), paused={}\n",
			                   __FUNCTION__,
			                   _nConsumed,
			                   _nConsumed / runningJob->status.framerate,
			                   paused);
		}

		if (!paused && !shouldKill)
		{
			runningJob->mgr->OnDevicePresent(device, runningJob->result->stat);
			runningJob->status.nFramesConsumed.fetch_add(1, std::memory_order_release);
			shouldKill |= !runningJob->result->stat.Ok();
		}
		runningJob->IncrementElapsedTime(paused);
	}

	if (shouldKill)
		SptAutoRender::StopMovieJob();
}

void AutoRenderFeature::ProcessDeferredJob(IDirect3DDevice9* device,
                                           std::shared_ptr<ArDeferredMovieJob> deferredJob,
                                           std::shared_ptr<ArRunningMultiDemoJob> fromMultiDemoJob)
{
	if (!deferredJob)
		return;

	// mY bOdy Is a mAChInE tHaT tUrnS deFeRreD jObS iNtO rUnNinG jObS

	auto startTime = ar_elapsed_time_clock::now();

	// init the running job object

	std::shared_ptr<ArRunningJob> newRunningJob = std::make_shared<ArRunningJob>();
	newRunningJob->status.startTime = startTime;
	newRunningJob->status.recordWhenConsoleIsOpen = deferredJob->recordWhenConsoleIsOpen;
	newRunningJob->status.framerate = deferredJob->framerate;
	// newRunningJob->controller = std::move(deferredJob->controller); // TODO remove
	newRunningJob->volume = deferredJob->volume;
	newRunningJob->captureAudio = deferredJob->captureAudio && SptAutoRender::SupportsAudioCapture();
	newRunningJob->recordAfterImGuiCallbacks = deferredJob->recordAfterImGuiCallbacks;
	newRunningJob->initializedFromMultiDemo = !!fromMultiDemoJob;

	// create consumer (init pipe, ffmpeg process, nutlib, etc.)

	auto& stat = newRunningJob->result->stat;
	std::optional<std::string> gameConCmd;

	/*
	* This here is some botched code to override some of the global placeholder values before
	* calling FormatString:
	* - create a copy of the global placeholders list (just a vector of pointers)
	* - create a copy of the placeholders we'll override (this just sets the key) and set them
	* - swap out the placeholders in our copy of the list
	* - call FormatString
	*/

	auto placeholders = ArGlobalPlaceholders::GetAll();

	ArPlaceholder demoSeqPh = ArGlobalPlaceholders::DEMO_SEQ.CopyByKey();
	ArPlaceholder demoNamePh = ArGlobalPlaceholders::DEMO_NAME.CopyByKey();

	auto swapPh = [&placeholders](const ArPlaceholder& repl)
	{
		auto it = std::ranges::find(placeholders, repl.key, &ArPlaceholder::key);
		*it = &repl;
	};

	if (fromMultiDemoJob)
	{
		swapPh(fromMultiDemoJob->phDatetimeReplacement);
		auto& multiDemoStatus = fromMultiDemoJob->status;

		size_t demoIdx = multiDemoStatus.nextDemoIdx.fetch_add(1, std::memory_order_acq_rel);
		demoSeqPh.SetValue(std::format("_{:04d}", demoIdx));
		swapPh(demoSeqPh);

		// TODO test with non-ascii paths
		auto& demoFileName = multiDemoStatus.demoFilePaths[demoIdx];
		demoNamePh.SetValue("_" + demoFileName.stem().string());
		swapPh(demoNamePh);

		gameConCmd = "playdemo \"" + demoFileName.string() + "\"";
	}

	ArPlaceholder::writeLock.lock();

	if (deferredJob->dumpDebugTimingFile)
	{
		std::string utf8FilePath = std::format("{}/{}_timing_dump.txt",
		                                       *ArGlobalPlaceholders::RENDER_WORKING_DIR.GetValue(),
		                                       *ArGlobalPlaceholders::DATE_TIME.GetValue());
		newRunningJob->timingInfoFile.open(ArUtf8ToUtf16(utf8FilePath.c_str()));
	}

	std::string utf8CmdLine = ArPlaceholder::FormatString(placeholders, deferredJob->unformattedCmdLine, nullptr);

	ArFfmpegWriter::InitArgs ffmpegInitArgs{
	    .ffmpegWorkingDir = ArUtf8ToUtf16(ArGlobalPlaceholders::RENDER_WORKING_DIR.GetValue()->c_str()),
	    .cmd = ArUtf8ToUtf16(utf8CmdLine.c_str()),
	    .pipeName = ArUtf8ToUtf16(ArGlobalPlaceholders::PIPE_NAME.GetValue()->c_str()),
	    .width = (size_t)atoi(ArGlobalPlaceholders::VID_WIDTH.GetValue()->c_str()),
	    .height = (size_t)atoi(ArGlobalPlaceholders::VID_HEIGHT.GetValue()->c_str()),
	    .framerate = (float)atof(ArGlobalPlaceholders::FRAMERATE.GetValue()->c_str()),
	    .captureAudio = newRunningJob->captureAudio,
	};

	ArPlaceholder::writeLock.unlock();

	std::unique_ptr<ArLockableSurfaceConsumer> consumer =
	    std::make_unique<ArFfmpegWriter>(ffmpegInitArgs, newRunningJob->result->returnCode, stat);

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

	{
		// TODO for super short demos, the cvars aren't reset...

		// kill old job and reset cvars

		std::lock_guard lk(sharedRunningJobMtx);

		StopMovieJobNoLock();

		// load new cvars, set moviename and swap out the running job object
		std::ranges::copy(deferredJob->cvars, std::back_inserter(newRunningJob->cvarStorage));
		if (newRunningJob->captureAudio)
		{
			// TODO document why the hell we need to do this
			*cl_movieinfo.type = 0;
			strcpy(cl_movieinfo.moviename, "spt_autorender");
		}
		newRunningJob->IncrementElapsedTime(false);
		runningMovieJob.store(newRunningJob);
	}

	// execute e.g. playdemo
	if (gameConCmd.has_value())
		interfaces::_engine_client->ClientCmd(gameConCmd->c_str());

	ArGlobalPlaceholders::UUID.Regenerate();
	ArGlobalPlaceholders::PIPE_NAME.Regenerate();
}

void AutoRenderFeature::StoreMovieJobResult(ArRunningJob& runningJob)
{
	// fill out runningJob->result and save it
	auto& result = runningJob.result;
	result->elapsedTime = runningJob.GetElapsedTime(true);
	result->unpausedElapsedTime = runningJob.GetElapsedTime(false);
	result->nFramesConsumed = runningJob.status.nFramesConsumed.load(std::memory_order_acquire);
	lastMovieJobResult.store(std::move(result), std::memory_order_release);
}
