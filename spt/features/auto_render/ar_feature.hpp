#pragma once

#include "spt/feature.hpp"
#include "ar_jobs.hpp"

#include <memory>
#include <mutex>

struct ArRunningJob;
struct IDirect3DDevice9;

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
};

class AutoRenderFeature : public FeatureWrapper<AutoRenderFeature>
{
public:
protected:
	virtual void InitHooks() override;
	virtual void LoadFeature() override;
	virtual void UnloadFeature() override;

private:
	static inline std::atomic<bool> imGuiCallbackActive = false;
	static inline std::atomic<bool> queuedKillSignal = false;

	// using atomic<shared_ptr> pattern to avoid mutexes
	static inline std::atomic<std::shared_ptr<ArDeferredMovieJob>> deferredMovieJob;
	static inline std::atomic<std::shared_ptr<ArRunningJob>> runningMovieJob; // only written on render thread
	static inline std::atomic<std::shared_ptr<ArAppResult>> lastAppResult;

	void OnShaderDevicePresentSignal(IDirect3DDevice9* device);
	void ImGuiTabCallback();

	struct portable_samplepair_t
	{
		int left;
		int right;
	};

	DECL_STATIC_HOOK_CDECL(void,
	                       S_TransferStereo16,
	                       void* pOutput,
	                       const portable_samplepair_t* pfront,
	                       int lpaintedtime,
	                       int endtime);

} static spt_auto_render_feat;
