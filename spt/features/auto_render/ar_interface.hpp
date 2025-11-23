#pragma once

#include "spt/feature.hpp"
#include "spt/utils/serialize.hpp" // TODO move ser::status to separate file

#include <memory>
#include <mutex>
#include <variant>
#include <string>

struct IDirect3DDevice9;

struct ArCvarSetting
{
	std::variant<std::string, ConVar*> cvar; // if string, will be looked up
	std::string val;
};

enum ArSyncMode
{
	AR_SYNC_FULL, // synchronous
	AR_SYNC_THREADED,
};

/*
* When this is converted into a running job, some values are grabbed from the global placeholders
* and the cmdLine is pumped through ArPlaceholder::FormatString().
*/
struct ArDeferredMovieJob
{
	std::string unformattedCmdLine; // utf8, will be converted to utf16
	ArSyncMode syncMode;
	std::optional<size_t> nFramesInFlight; // only used if asyncMode != AR_SYNC_FULL, reasonable default is 3
	// TODO the user can call StopMovieJob themselves, this isn't necessary
	std::vector<ArCvarSetting> cvars;
	float volume;
	float framerate;
	bool captureAudio;
	bool recordWhenConsoleIsOpen;   // TODO now that i'm using startmovie logic, remove this
	bool recordAfterImGuiCallbacks; // do you want ImGui to show up in the video?
	bool dumpDebugTimingFile; // if set, creates a dump file in the ffmpeg working dir
};

using ar_elapsed_time_clock = std::chrono::steady_clock;

struct ArRunningMovieJobStatus
{
	std::atomic<size_t> nFramesConsumed;
	std::atomic<size_t> nAudioSamplesConsumed;
	float framerate;
	ar_elapsed_time_clock::time_point startTime;
	std::atomic<ar_elapsed_time_clock::duration> unpausedElapsedTime;
	bool recordWhenConsoleIsOpen;
};

struct ArRunningMultiDemoJobStatus
{
	std::vector<std::filesystem::path> demoFilePaths;
	/*
	* e.g. 0 -> not started yet, 1 -> first demo running or finished, etc.
	* May be greater than demoFilePaths.size() if on last demo.
	*/
	std::atomic<size_t> nextDemoIdx;
	ar_elapsed_time_clock::time_point startTime;
};

struct ArMovieJobResult
{
	size_t nFramesConsumed; // if the process crashed, this may be slightly less than the actual video length
	std::optional<DWORD> returnCode; // set if the process was even created
	ar_elapsed_time_clock::duration elapsedTime;
	ar_elapsed_time_clock::duration unpausedElapsedTime;
	ser::StatusTracker stat;
};

/*
* Provides functionality to automatically render videos by streaming raw data to ffmpeg. Since this
* happens over many frames and launches an external process, we have a bit of lifetime management:
* 
* 1) start by submitting a job via QueueSingleMovieJob() - this returns immediately
* 2) the job starts on the next frame, and you can query its status via GetRunningJobStatus()
* 3) the feature feeds in raw frames and audio to ffmpeg every frame
* 4) the job stops if:
*      - ffmpeg exits prematurely
*      - you call StopMovieJob() - this stops streaming data, but ffmpeg may take a while to finish
*      - the maximum number of frames is reached (if set)
* 5) once the job stops, you can query the result via GetLastMovieJobResult()
* 
* Note that there is at most only ever one pending, one running, and one finished job at a time.
* Anyone can call these functions which may override the previous state. A running job corresponds
* one-to-one with a video output.
* 
* Most of this data is shared between multiple threads, so it *should* be thread safe. However, it
* is designed to be called from the main game thread (e.g. framesignal, concmd, imgui, etc.).
*/

class SptAutoRender
{
public:
	// available during or after LoadFeature
	static bool Works();
	static bool MultiDemoJobWorks();
	static bool SupportsAudioCapture();

	/*
	* Replaces the currently queued job (if any), and attempts to create a running job on the next frame.
	* - if initialization fails:
	*     * the currently running job (if any) continues unaffected
	*     * the status of the failed job is stored (GetLastMovieJobResult())
	* - if initialization succeeds:
	*     * any currently running job (if any) is stopped
	*     * the status of the running job is stored (GetLastMovieJobResult())
	*     * a new running job is created (GetRunningMovieJobStatus())
	* 
	* Returns true if the job was queued, false if there is already a multi-demo job running.
	*/
	static bool QueueSingleMovieJob(std::unique_ptr<ArDeferredMovieJob> deferred);

	/*
	* A wrapper of QueueSingleMovieJob() that can be used to render a list of demos. This will
	* effectively automatically call QueueSingleMovieJob() for each demo in the list. You can query
	* its status via GetRunningMultiDemoJobStatus(). If you attempt to use the single-job functions
	* while this is running:
	* - if QueueSingleMovieJob succeeds, it will destroy the multi-demo job
	* - a call to QueueMultiDemoJob is a noop
	* - StopMovieJob will only stop the current demo
	* 
	* This will automatically call QueueSingleMovieJob() with the templateDeferredJob until the
	* list of demos is exhausted. Returns true if the job was queued, false if there is any other
	* type of job running. This will also stop the current running single movie job (if any).
	*/
	static bool QueueMultiDemoJob(std::unique_ptr<ArDeferredMovieJob> templateDeferredJob,
	                              std::vector<std::filesystem::path> demoFilePaths);

	static std::shared_ptr<const ArRunningMovieJobStatus> GetRunningMovieJobStatus();
	static std::shared_ptr<const ArRunningMultiDemoJobStatus> GetRunningMultiDemoJobStatus();
	static std::shared_ptr<const ArMovieJobResult> GetLastMovieJobResult();
	// blocks and returns true if a job was stopped
	static bool StopMovieJob();
	// calls StopMovieJob as well if there is a multi-demo job
	static bool StopMultiDemoJob();
};
