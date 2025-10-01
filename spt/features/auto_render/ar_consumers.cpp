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

ArFfmpegWriter::ArFfmpegWriter(InitArgs& args, std::optional<DWORD>& procReturnCode, ser::StatusTracker& stat)
    : ffmpegReturnCode(procReturnCode)
{
	std::error_code ec;
	std::filesystem::create_directories(args.ffmpegWorkingDir, ec);
	if (ec)
	{
		stat.Err("[" __FUNCTION__ "]: failed to create working dir");
		return;
	}

	/*struct
	{
		HANDLE& handle;
		const wchar* name;
		size_t initSize;
	} pipeInfos[2]{
	    {videoPipe, args.videoPipeName.c_str(), args.width * args.height * 4 * 20},
	    {audioPipe, args.audioPipeName.c_str(), (size_t)(44100.f / args.framerate * sizeof(short) * 2)},
	};

	for (auto& pipeInfo : pipeInfos)
	{
		if (!pipeInfo.name)
			continue;
		// TODO make overlapped to allow for abort
		pipeInfo.handle = CreateNamedPipeW(pipeInfo.name,
		                                   PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
		                                   PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_ACCEPT_REMOTE_CLIENTS,
		                                   1,
		                                   pipeInfo.initSize,
		                                   0,
		                                   0,
		                                   nullptr);
		if (pipeInfo.handle == INVALID_HANDLE_VALUE)
		{
			stat.Err(std::format("[{}]: CreateNamedPipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
			return;
		}
	}*/

	pipe = CreateNamedPipeW(args.pipeName.c_str(),
	                        PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
	                        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_ACCEPT_REMOTE_CLIENTS,
	                        1,
	                        args.width * args.height * 4 * 2,
	                        0,
	                        0,
	                        nullptr);
	if (pipe == INVALID_HANDLE_VALUE)
	{
		stat.Err(std::format("[{}]: CreateNamedPipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
		return;
	}

	STARTUPINFOW si = {
	    .cb = sizeof(si),
	};

	if (!CreateProcessW(nullptr,
	                    args.cmd.data(),
	                    nullptr,
	                    nullptr,
	                    FALSE,
	                    CREATE_NO_WINDOW,
	                    nullptr,
	                    args.ffmpegWorkingDir.c_str(),
	                    &si,
	                    &ffmpegProc))
	{
		stat.Err(std::format("[{}]: CreateProcess failed: {}", __FUNCTION__, ArLastErrorAsString()));
		return;
	}

	procValid = true;

	// TODO test at lower framerate values, check if audio gets desynced
	// TODO round framerate to 0.001 in imgui

	// TODO pass in placeholders to here

	// you can get the fourcc codes from https://git.ffmpeg.org/gitweb/nut.git/blob/HEAD:/docs/nut4cc.txt

	// video must be index 0, audio must be index 1
	nut_stream_header_tt sh[3]{
	    {
	        .type = NUT_VIDEO_CLASS,
	        .fourcc_len = 4,
	        .fourcc = (uint8_t*)"BGR\0",
	        .time_base{.num = 1000, .den = (int)(args.framerate * 1000)},
	        .fixed_fps = 1,
	        .width = (int)args.width,
	        .height = (int)args.height,
	        .sample_width = 1, // TODO can I just set both of these to 0?
	        .sample_height = 1,
	    },
	    {
	        .type = NUT_AUDIO_CLASS,
	        .fourcc_len = 4,
	        .fourcc = (uint8_t*)"PSD\x10",
	        .time_base{.num = 1, .den = 44100},
	        .samplerate_num = 44100,
	        .samplerate_denom = 1,
	        .channel_count = 2,
	    },
	    {
	        .type = -1,
	    },
	};

	nut_muxer_opts_tt opts{
	    .output{.priv = this, .write = &ArFfmpegWriter::FfmpegWrite},
	    .realtime_stream = 1,
	    .max_distance = 32768,
	};

	context = nut_muxer_init(&opts, sh, nullptr);
}

int ArFfmpegWriter::FfmpegWrite(void* priv, size_t len, const uint8_t* buf)
{
	ArFfmpegWriter* thisptr = static_cast<ArFfmpegWriter*>(priv);

	if (!thisptr->writerStat.Ok())
		return 0;

	if (!thisptr->pipeConnected)
	{
		BOOL connected =
		    ConnectNamedPipe(thisptr->pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
		if (!connected)
		{
			thisptr->writerStat.Err(
			    std::format("[{}]: ConnectNamedPipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
			return 0;
		}
		thisptr->pipeConnected = true;
	}

	DWORD nWritten;
	if (!WriteFile(thisptr->pipe, buf, len, &nWritten, nullptr) || nWritten != len)
	{
		thisptr->writerStat.Err(
		    std::format("[{}]: WriteFile for pipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
		return nWritten;
	}

	return len;
}

// TODO misspelling -pixel_format hangs somewhere...
void ArFfmpegWriter::Consume(D3DLOCKED_RECT rect,
                             const D3DSURFACE_DESC& desc,
                             ar_frame_idx idx,
                             ser::StatusTracker& stat)
{
	/*DWORD exitCode;
	if (!!GetExitCodeProcess(ffmpegProc.hProcess, &exitCode) && exitCode != STILL_ACTIVE)
	{
		stat.Err("[" __FUNCTION__ "]: the process has stopped prematurely");
		return;
	}*/
	if (desc.Format != D3DFMT_A8R8G8B8)
	{
		stat.Err("[" __FUNCTION__ "]: unexpected surface format");
		return;
	}

	/*BOOL connected = ConnectNamedPipe(videoPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
	if (!connected)
	{
		stat.Err(std::format("[{}]: ConnectNamedPipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
		return;
	}

	if ((DWORD)rect.Pitch == desc.Width * 4)
	{
		// write full frame
		DWORD nToWrite = desc.Width * desc.Height * 4;
		DWORD nWritten;
		if (!WriteFile(videoPipe, rect.pBits, nToWrite, &nWritten, nullptr) || nWritten != nToWrite)
			stat.Err(
			    std::format("[{}]: WriteFile for pipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
	}
	else
	{
		// write row by row
		DWORD nToWrite = rect.Pitch;
		DWORD nWritten;
		for (DWORD i = 0; i < desc.Height; i++)
		{
			if (!WriteFile(videoPipe, (std::byte*)rect.pBits + rect.Pitch * i, nToWrite, &nWritten, nullptr)
			    || nWritten != nToWrite)
			{
				stat.Err(std::format("[{}]: WriteFile for pipe failed: {}",
				                     __FUNCTION__,
				                     ArLastErrorAsString()));
				return;
			}
		}
	}*/

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

	// TODO check for error
	std::lock_guard lk(nutLock);
	if (context)
		nut_write_frame_reorder(context, &pkt, (const uint8_t*)rect.pBits);
	if (!writerStat.Ok())
		stat.Concat(std::move(writerStat));
}

void ArFfmpegWriter::StopFfmpeg()
{
	/*std::reference_wrapper<HANDLE> namedPipes[2] = {videoPipe, audioPipe};
	for (HANDLE& namedPipe : namedPipes)
	{
		if (namedPipe == INVALID_HANDLE_VALUE)
			continue;
		FlushFileBuffers(namedPipe);
		DisconnectNamedPipe(namedPipe);
		CloseHandle(namedPipe);
		namedPipe = INVALID_HANDLE_VALUE;
	}*/

	{
		std::lock_guard lk(nutLock);
		nut_muxer_uninit_reorder(context);
		context = nullptr;
	}

	if (pipeConnected)
	{
		FlushFileBuffers(pipe);
		DisconnectNamedPipe(pipe);
	}
	CloseHandle(pipe);
	pipe = INVALID_HANDLE_VALUE;

	if (procValid)
	{
		WaitForSingleObject(ffmpegProc.hProcess, INFINITE);
		ffmpegReturnCode = -1;
		if (!GetExitCodeProcess(ffmpegProc.hProcess, &ffmpegReturnCode.value()))
			ffmpegReturnCode.reset();
		CloseHandle(ffmpegProc.hProcess);
		CloseHandle(ffmpegProc.hThread);
		procValid = false;
	}
}

void ArFfmpegWriter::ConsumeAudio(const short* lrPcmSamples, size_t nSamplePairs, ser::StatusTracker& stat)
{
	/*BOOL connected = ConnectNamedPipe(audioPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
	if (!connected)
	{
		stat.Err(std::format("[{}]: ConnectNamedPipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
		return;
	}
	DWORD nToWrite = nSamples * sizeof(*lrPcmSamples);
	DWORD nWritten;
	if (!WriteFile(audioPipe, lrPcmSamples, nToWrite, &nWritten, nullptr) || nWritten != nToWrite)
		stat.Err(std::format("[{}]: WriteFile for pipe failed: {}", __FUNCTION__, ArLastErrorAsString()));*/

	nut_packet_tt pkt{
	    .len = (int)(nSamplePairs * 2 * sizeof(*lrPcmSamples)),
	    .stream = 1,
	    .pts = (uint64_t)nAudioSamplePairsWritten,
	    .flags = NUT_FLAG_KEY,
	    .next_pts = nAudioSamplePairsWritten + nSamplePairs,
	};
	nAudioSamplePairsWritten += nSamplePairs;

	// TODO check for error
	std::lock_guard lk(nutLock);
	if (context)
		nut_write_frame_reorder(context, &pkt, (const uint8_t*)lrPcmSamples);
}

void ArFfmpegWriter::Finish()
{
	StopFfmpeg();
}

ArFfmpegWriter::~ArFfmpegWriter()
{
	StopFfmpeg();
}
