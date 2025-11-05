#include "stdafx.hpp"

#include "ar_jobs.hpp"
#include "ar_util.hpp"

void ArLockableSurfaceConsumer::LockAndConsume(IDirect3DSurface9* offScreenSurface,
                                               size_t seq,
                                               ser::StatusTracker& stat)
{
	HRESULT hr;
	D3DSURFACE_DESC desc;
	D3DLOCKED_RECT rect;

	if (!stat.Ok())
		return;

	hr = offScreenSurface->GetDesc(&desc);
	if (FAILED(hr))
	{
		stat.Err(std::format("[{}]: IDirect3DSurface9::GetDesc failed for sequence {}", __FUNCTION__, seq));
		return;
	}

	hr = offScreenSurface->LockRect(&rect, nullptr, D3DLOCK_READONLY);
	if (FAILED(hr))
	{
		stat.Err(std::format("[{}]: IDirect3DSurface9::LockRect failed for sequence {}", __FUNCTION__, seq));
		return;
	}

	Consume(rect, desc, seq, stat);
	if (!stat.Ok())
		return;

	hr = offScreenSurface->UnlockRect();
	if (FAILED(hr))
	{
		stat.Err(std::format("[{}]: IDirect3DSurface9::UnlockRect failed for sequence {}", __FUNCTION__, seq));
		return;
	}
}

// TODO add option for pipe timeout

#define AR_DO_NUTLIB_CALL(func_with_args, stat_) \
	do \
	{ \
		std::unique_lock nutLibLk(nutLibMtx); \
		nutLibUData.stat = &stat_; \
		func_with_args; \
		nutLibUData.stat = nullptr; \
	} while (0)

ArFfmpegWriter::ArFfmpegWriter(const InitArgs& args, std::optional<DWORD>& procReturnCode, ser::StatusTracker& stat)
    : captureAudio(args.captureAudio), ffmpegReturnCode(procReturnCode)
{
	// create working dir

	std::error_code ec;
	std::filesystem::create_directories(args.ffmpegWorkingDir, ec);
	if (ec)
	{
		stat.Err("[" __FUNCTION__ "]: failed to create working dir");
		return;
	}

	// create pipe

	hPipe = CreateNamedPipeW(args.pipeName.c_str(),
	                         PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
	                         PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_ACCEPT_REMOTE_CLIENTS,
	                         1,
	                         args.width * args.height * 4 * 3, // set pipe size to 3 frames
	                         0,
	                         0,
	                         nullptr);
	if (hPipe == INVALID_HANDLE_VALUE)
	{
		stat.Err(std::format("[{}]: CreateNamedPipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
		hPipe.reset();
		return;
	}

	// create overlapped events

	for (auto& ov : overlappedArr)
	{
		ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (ov.hEvent == NULL)
		{
			stat.Err(std::format("[{}]: CreateEventW failed: {}", __FUNCTION__, ArLastErrorAsString()));
			return;
		}
	}

	// init process

	STARTUPINFOW si = {
	    .cb = sizeof(si),
	};

	auto cmdCopy = args.cmd; // cmdLine must be modifyable, make a copy
	ffmpegProc.emplace();
	if (!CreateProcessW(nullptr,
	                    cmdCopy.data(),
	                    nullptr,
	                    nullptr,
	                    FALSE,
	                    CREATE_NO_WINDOW,
	                    nullptr,
	                    args.ffmpegWorkingDir.c_str(),
	                    &si,
	                    &ffmpegProc.value()))
	{
		stat.Err(std::format("[{}]: CreateProcess failed: {}", __FUNCTION__, ArLastErrorAsString()));
		ffmpegProc.reset();
		return;
	}

	// TODO test at lower framerate values, check if audio gets desynced
	// TODO round framerate to 0.001 in imgui

	// you can get the fourcc codes from https://git.ffmpeg.org/gitweb/nut.git/blob/HEAD:/docs/nut4cc.txt

	int i = 0;
	std::array<nut_stream_header_tt, 3> sh;

	// video must be index 0, audio must be index 1

	sh[i++] = nut_stream_header_tt{
	    .type = NUT_VIDEO_CLASS,
	    .fourcc_len = 4,
	    .fourcc = (uint8_t*)"BGR\0",
	    .time_base{.num = 100000, .den = (int)(args.framerate * 100000)},
	    .fixed_fps = 1,
	    .width = (int)args.width,
	    .height = (int)args.height,
	    .sample_width = 1, // TODO can I just set both of these to 0?
	    .sample_height = 1,
	};

	if (args.captureAudio)
	{
		sh[i++] = nut_stream_header_tt{
		    .type = NUT_AUDIO_CLASS,
		    .fourcc_len = 4,
		    .fourcc = (uint8_t*)"PSD\x10",
		    .time_base{.num = 1, .den = 44100},
		    .samplerate_num = 44100,
		    .samplerate_denom = 1,
		    .channel_count = 2,
		};
	}

	sh[i++].type = -1;

	nut_muxer_opts_tt opts{
	    .output{.priv = &nutLibUData, .write = &ArFfmpegWriter::FfmpegWrite},
	    .realtime_stream = 1,
	    .max_distance = 32768,
	};

	AR_DO_NUTLIB_CALL(context = nut_muxer_init(&opts, sh.data(), nullptr), stat);
}

int ArFfmpegWriter::FfmpegWrite(void* priv, size_t len, const uint8_t* buf)
{
	NutLibWriterUserData* uData = static_cast<NutLibWriterUserData*>(priv);
	Assert(!!uData && !!uData->thisptr && !!uData->stat);
	return uData->thisptr->FfmpegWriteImpl(len, buf, *uData->stat);
}

int ArFfmpegWriter::FfmpegWriteImpl(size_t len, const uint8_t* buf, ser::StatusTracker& stat)
{
	if (!stat.Ok())
		return 0;

	EnsurePipeConnected(stat);
	if (!stat.Ok())
		return 0;

	// TODO implement async writes properly, uhhhhh can I????
	// I guess just use one OVERLAPPED struct

	// allow up to overlappedArr.size() async writes at a time
	LPOVERLAPPED ov = &overlappedArr[overlappedWriteIdx % overlappedArr.size()];
	/*if (overlappedWriteIdx >= overlappedArr.size())
	{
		// wait for previous write to finish
		BOOL waitRet = WaitForSingleObject(ov->hEvent, INFINITE);
		if (waitRet != WAIT_OBJECT_0)
		{
			stat.Err(std::format("[{}]: WaitForSingleObject for async write failed: {}",
			                     __FUNCTION__,
			                     ArLastErrorAsString()));
			return 0;
		}
	}*/
	overlappedWriteIdx++;
	BOOL writeRet = WriteFile(hPipe.value(), buf, len, nullptr, ov);
	if (!writeRet && GetLastError() != ERROR_IO_PENDING)
	{
		stat.Err(std::format("[{}]: WriteFile for async pipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
		return 0;
	}

	// TODO wait synchronously for the write to finish (data must remain valid for entire write)
	BOOL waitRet = WaitForSingleObject(ov->hEvent, INFINITE);
	if (waitRet != WAIT_OBJECT_0)
	{
		stat.Err(std::format("[{}]: WaitForSingleObject for async write failed: {}",
		                     __FUNCTION__,
		                     ArLastErrorAsString()));
		return 0;
	}

	return len;
}

void ArFfmpegWriter::EnsurePipeConnected(ser::StatusTracker& stat)
{
	if (pipeConnected)
		return;

	// connect synchronously

	constexpr DWORD connectTimeout = 1000;

	LPOVERLAPPED overlapped = &overlappedArr[0];
	ConnectNamedPipe(hPipe.value(), overlapped);
	DWORD err = GetLastError();
	if (err == ERROR_IO_PENDING)
	{
		// if ffmpeg gets an unknown option it'll stop before it connects to the pipe
		DWORD waitRet = WaitForSingleObject(overlapped->hEvent, connectTimeout);
		if (waitRet == WAIT_TIMEOUT)
			AR_STAT_FUNC_ERR_V(stat, "ConnectNamedPipe timed out after {}ms", connectTimeout);
		if (waitRet != WAIT_OBJECT_0)
			AR_STAT_FUNC_WIN_ERR(stat, "WaitForSingleObject for ConnectNamedPipe failed");
	}
	else if (err != ERROR_PIPE_CONNECTED)
	{
		AR_STAT_FUNC_WIN_ERR(stat, "ConnectNamedPipe failed");
	}

	for (auto& ov : overlappedArr)
		SetEvent(ov.hEvent); // reset all events to signalled before writing

	pipeConnected = stat.Ok();
}

// TODO misspelling -pixel_format hangs somewhere...
void ArFfmpegWriter::Consume(D3DLOCKED_RECT rect,
                             const D3DSURFACE_DESC& desc,
                             size_t idx,
                             ser::StatusTracker& stat)
{
	std::shared_lock destroyLk(destroyMtx);

	if (!context)
		return;

	if (desc.Format != D3DFMT_A8R8G8B8)
	{
		AR_STAT_FUNC_ERR(stat, "unexpected surface format");
		return;
	}

	if ((DWORD)rect.Pitch != desc.Width * 4)
	{
		stat.Err("[" __FUNCTION__ "]: unexpected surface pitch");
		return;
	}

	nut_packet_tt pkt{
	    .len = (int)(desc.Width * desc.Height * 4),
	    .stream = 0,
	    .pts = idx,
	    .flags = NUT_FLAG_KEY,
	    .next_pts = idx + 1,
	};

	AR_DO_NUTLIB_CALL(nut_write_frame_reorder(context, &pkt, (const uint8_t*)rect.pBits), stat);
}

void ArFfmpegWriter::ConsumeAudio(const short* lrPcmSamples, size_t nSamplePairs, ser::StatusTracker& stat)
{
	std::shared_lock destroyLk(destroyMtx);

	Assert(captureAudio);
	if (!captureAudio || !context)
		return;

	nut_packet_tt pkt{
	    .len = (int)(nSamplePairs * 2 * sizeof(*lrPcmSamples)),
	    .stream = 1,
	    .pts = (uint64_t)nAudioSamplePairsWritten,
	    .flags = NUT_FLAG_KEY,
	    .next_pts = nAudioSamplePairsWritten + nSamplePairs,
	};
	nAudioSamplePairsWritten += nSamplePairs;

	AR_DO_NUTLIB_CALL(nut_write_frame_reorder(context, &pkt, (const uint8_t*)lrPcmSamples), stat);
}

void ArFfmpegWriter::StopFfmpeg()
{
	std::unique_lock destroyLk(destroyMtx);

	// flush & kill nutlib

	ser::StatusTracker tmpStat;
	AR_DO_NUTLIB_CALL(nut_muxer_uninit_reorder(context), tmpStat);
	context = nullptr;

	// flush pipe

	if (pipeConnected)
		FlushFileBuffers(hPipe.value()); // this should be sufficient instead of waiting for all async write

	for (auto& ov : overlappedArr)
	{
		if (ov.hEvent != NULL)
		{
			CloseHandle(ov.hEvent);
			ov.hEvent = nullptr;
		}
	}

	if (pipeConnected)
	{
		DisconnectNamedPipe(hPipe.value());
		pipeConnected = false;
	}

	// kill pipe

	if (hPipe.has_value())
	{
		CloseHandle(hPipe.value());
		hPipe.reset();
	}

	// kill process

	if (ffmpegProc.has_value())
	{
		WaitForSingleObject(ffmpegProc.value().hProcess, INFINITE);
		ffmpegReturnCode = -1;
		if (!GetExitCodeProcess(ffmpegProc.value().hProcess, &ffmpegReturnCode.value()))
			ffmpegReturnCode.reset();
		CloseHandle(ffmpegProc.value().hProcess);
		CloseHandle(ffmpegProc.value().hThread);
		ffmpegProc.reset();
	}
}

void ArFfmpegWriter::Finish()
{
	StopFfmpeg();
}

ArFfmpegWriter::~ArFfmpegWriter()
{
	StopFfmpeg();
}
