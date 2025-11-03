#pragma once

#include "spt/feature.hpp"
#include "ar_jobs.hpp"
#include "ar_placeholders.hpp"

#include <memory>
#include <mutex>
#include <variant>
#include <string>

struct IDirect3DDevice9;
class ArMovieController;

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

struct ArDeferredMovieJob
{
	/*
	* Most of the stuff from ffmpegArgs is copied straight from the global placeholders, but it
	* must be provided here because of possible race conditions. Here's an example:
	* 
	* 1) say the cmdLine includes a UUID as part of the pipe name
	* 2) the ffmpeg process isn't started right away; it starts on the next frame
	* 3) before that happens, the value of the UUID placeholder is changed
	* 4) the job opens a pipe with a different name than that of the cmdLine
	* 
	* TODO there should be some publicly exposed function to convert from placeholders to ffmpegArgs
	* TODO why don't I pass everything in as a list of placeholders again?
	*/
	ArFfmpegWriter::InitArgs ffmpegArgs;
	ArSyncMode syncMode;
	std::optional<size_t> nFramesInFlight; // only used if asyncMode != AR_SYNC_FULL, reasonable default is 3
	std::unique_ptr<ArMovieController> controller; // optional controller to manage stopping conditions
	std::vector<ArCvarSetting> cvars;
	float volume;
	bool recordWhenConsoleIsOpen;   // TODO now that i'm using startmovie logic, remove this
	bool recordAfterImGuiCallbacks; // do you want ImGui to show up in the video?
};

using ar_elapsed_time_clock = std::chrono::steady_clock;

struct ArRunningMovieJobStatus
{
	std::atomic<ar_frame_idx> nFramesConsumed;
	float outputFramerate;
	ar_elapsed_time_clock::time_point startTime;
	std::atomic<ar_elapsed_time_clock::duration> unpausedElapsedTime;
	std::atomic<bool> userPaused;
	bool recordWhenConsoleIsOpen;
};

struct ArMovieJobResult
{
	ar_frame_idx nFramesConsumed; // if the process crashed, this may be slightly less than the actual video length
	std::optional<DWORD> returnCode; // set if the process was even created
	ar_elapsed_time_clock::duration elapsedTime;
	ar_elapsed_time_clock::duration unpausedElapsedTime;
	ser::StatusTracker stat;
};

class ArMovieController
{
public:
	// will be called every frame after at least 1 video frame has been consumed
	virtual bool ShouldStopRecording(const ArRunningMovieJobStatus& status) = 0;
	virtual ~ArMovieController() = default;
};

/*
* Provides functionality to automatically render videos by streaming raw data to ffmpeg. Since this
* happens over many frames and launches an external process, we have a bit of lifetime management:
* 
* 1) start by submitting a job via QueueMovieJob() - this returns immediately
* 2) the job starts on the next frame, and you can query its status via GetRunningJobStatus()
* 3) the feature feeds in raw frames and audio to ffmpeg every frame, you can pause and resume it via PauseMovieJob()
* 4) the job stops if:
*      - ffmpeg exits prematurely
*      - you call StopMovieJob() - this stops streaming data, but ffmpeg may take a while to finish
*      - the maximum number of frames is reached (if set)
* 5) once the job stops, you can query the result via GetLastMovieJobResult()
* 
* Note that there is at most only ever one pending, one running, and one finished job at a time.
* Anyone can call these functions which may override the previous state.
* 
* Most of this data is shared between multiple threads, so it *should* be thread safe. However, it
* is designed to be called from the main game thread (e.g. framesignal, concmd, imgui, etc.).
*/

class SptAutoRender
{
public:
	// available during or after LoadFeature
	static bool Works();
	static bool SupportsAudioCapture();

	// replaces any currently queued job
	static void QueueMovieJob(std::unique_ptr<ArDeferredMovieJob> deferred);
	static std::shared_ptr<const ArRunningMovieJobStatus> GetRunningMovieJobStatus();
	static std::shared_ptr<const ArMovieJobResult> GetLastMovieJobResult();
	static bool PauseMovieJob(bool pauseState); // returns true if a job is still running
	static bool StopMovieJob();                 // blocks and returns true if a job was stopped

};
