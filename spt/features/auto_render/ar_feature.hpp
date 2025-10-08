#pragma once

#include "spt/feature.hpp"
#include "ar_jobs.hpp"

#include <memory>
#include <mutex>

struct ArRunningJob;
struct IDirect3DDevice9;

// RAII class that saves & restores the value of a cvars
class ArCvarStorage
{
	ConVar* cvar;
	std::string oldVal;

public:
	ArCvarStorage(ConVar* cvar, const char* newVal) : cvar(cvar), oldVal(cvar->GetString())
	{
		if (cvar)
			cvar->SetValue(newVal);
	};

	ArCvarStorage(const char* cvarName, const char* newVal)
	    : ArCvarStorage(g_pCVar ? g_pCVar->FindVar(cvarName) : nullptr, newVal) {};

	ArCvarStorage(ArCvarStorage&) = delete;

	ArCvarStorage(ArCvarStorage&& o) : cvar(o.cvar), oldVal(std::move(o.oldVal))
	{
		o.cvar = nullptr;
	}

	~ArCvarStorage()
	{
		if (cvar)
			cvar->SetValue(oldVal.c_str());
	};
};

enum ArSyncMode : int
{
	AR_SYNC_FULL,
	AR_SYNC_ASYNC,
	AR_SYNC_THREADED,
};

struct ArAppResult
{
	ar_frame_idx nFramesConsumed; // if the process crashed, this may be slightly less than the actual video length
	std::optional<DWORD> returnCode; // set if the process was even created
	std::chrono::nanoseconds duration;
	ser::StatusTracker stat;
};

struct ArDeferredMovieJob
{
	ArFfmpegWriter::InitArgs procArgs;
	std::unique_ptr<ArAppResult> result;
	std::optional<ar_frame_idx> maxNFrames;
	ArSyncMode syncMode;
	size_t nFramesInFlight; // only used if asyncMode != AR_SYNC_FULL
	std::vector<ArCvarStorage> cvarStorage;
	float volume;
	bool captureAudio;
};

class AutoRenderFeature : public FeatureWrapper<AutoRenderFeature>
{
public:
	// available during or after LoadFeature
	bool SupportsAudioCapture();
	std::vector<ArCvarStorage> MakeDefaultCvarStorage(float hostFrameRateVal);

protected:
	virtual bool ShouldLoadFeature() override;
	virtual void InitHooks() override;
	virtual void LoadFeature() override;
	virtual void UnloadFeature() override;

private:
	inline static ConVar* snd_lockpartial = nullptr;
	static inline std::atomic<bool> imGuiCallbackActive = false;
	static inline std::atomic<bool> queuedKillSignal = false;

	// using atomic<shared_ptr> pattern to avoid mutexes
	static inline std::atomic<std::shared_ptr<ArDeferredMovieJob>> deferredMovieJob;
	static inline std::atomic<std::shared_ptr<ArRunningJob>> runningMovieJob; // only written on render thread
	static inline std::atomic<std::shared_ptr<ArAppResult>> lastAppResult;

	void OnShaderDevicePresentSignal(IDirect3DDevice9* device);
	void ImGuiTabCallback();

	DECL_STATIC_HOOK_THISCALL(void, CAudioDirectSound__TransferSamples, void*, int end);

	DECL_STATIC_HOOK_CDECL(void,
	                       S_TransferStereo16,
	                       void* pOutput,
	                       const void* pfront,
	                       int lpaintedtime,
	                       int endtime);

} static spt_auto_render_feat;
