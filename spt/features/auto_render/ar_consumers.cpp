#include "stdafx.hpp"

#include "ar_decls.hpp"
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

void tga_write(const char* path,
               uint32_t width,
               uint32_t height,
               uint8_t* dataBGRA,
               uint8_t dataChannels,
               uint8_t fileChannels,
               ser::StatusTracker& stat)
{
	FILE* f = fopen(path, "wb");
	if (!f)
	{
		stat.Err(std::format("[{}]: failed to open file '{}' for writing", __FUNCTION__, path));
		return;
	}

	// TGA header: http://www.paulbourke.net/dataformats/tga/
	// clang-format off
	uint8_t header[18] = { 0,0,2,0,0,0,0,0,0,0,0,0, (uint8_t)(width%256), (uint8_t)(width/256), (uint8_t)(height%256), (uint8_t)(height/256), (uint8_t)(fileChannels*8), 0x20 };
	// clang-format on
	fwrite(header, 1, 18, f);

	std::unique_ptr<uint8_t[]> data = std::make_unique_for_overwrite<uint8_t[]>(width * height * fileChannels);
	size_t x = 0;

	for (uint32_t i = 0; i < width * height; i++)
		for (uint32_t b = 0; b < fileChannels; b++)
			data.get()[x++] = dataBGRA[(i * dataChannels) + (b % fileChannels)];

	fwrite(data.get(), 1, width * height * fileChannels, f);
	fclose(f);
}

void ArTgaWriter::Consume(D3DLOCKED_RECT rect, const D3DSURFACE_DESC& desc, size_t seq, ser::StatusTracker& stat)
{
	if (desc.Format != D3DFMT_A8R8G8B8)
	{
		stat.Err(std::format("[{}]: unsupported surface format {}, only D3DFMT_A8R8G8B8 is supported",
		                     __FUNCTION__,
		                     (int)desc.Format));
		return;
	}

	if (singleFrame && hasWritten)
	{
		stat.Err(std::format("[{}]: expected only 1 frame but got more", __FUNCTION__));
		return;
	}

	std::string formattedPath;
	try
	{
		formattedPath = std::vformat(fmt, std::make_format_args(seq));
	}
	catch (const std::format_error& e)
	{
		stat.Err(std::format("[{}]: invalid format string: {}", __FUNCTION__, e.what()));
		return;
	}
	if (!singleFrame && formattedPath == fmt)
	{
		stat.Err(std::format(
		    "[{}]: format string does not contain any formatting specifiers for the frame number, use e.g. '{}'",
		    __FUNCTION__,
		    "my_frames_{:04d}.tga"));
		return;
	}

	tga_write(formattedPath.c_str(), desc.Width, desc.Height, (uint8_t*)rect.pBits, 4, 3, stat);
	if (!stat.Ok())
		return;

	if (openAfterWrite)
		ShellExecute(nullptr, nullptr, formattedPath.c_str(), nullptr, nullptr, SW_SHOW);

	hasWritten = true;
}

ArFfmpegWriter::ArFfmpegWriter(InitArgs& args, ser::StatusTracker& stat) : ffmpegReturnCode(args.ffmpegReturnCode)
{
	std::error_code ec;
	std::filesystem::create_directories(args.ffmpegWorkingDir, ec);
	if (ec)
	{
		stat.Err("[" __FUNCTION__ "]: failed to create working dir");
		return;
	}

	struct
	{
		HANDLE& handle;
		const wchar* name;
	} pipeInfos[2]{
	    {videoPipe, args.videoPipeName.c_str()},
	    {audioPipe, args.audioPipeName.c_str()},
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
		                                   args.width * args.height * 4,
		                                   0,
		                                   0,
		                                   nullptr);
		if (pipeInfo.handle == INVALID_HANDLE_VALUE)
		{
			stat.Err(std::format("[{}]: CreateNamedPipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
			return;
		}
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
}

void ArFfmpegWriter::Consume(D3DLOCKED_RECT rect,
                             const D3DSURFACE_DESC& desc,
                             ar_frame_idx idx,
                             ser::StatusTracker& stat)
{
	if (desc.Format != D3DFMT_A8R8G8B8)
	{
		stat.Err("[" __FUNCTION__ "]: unexpected surface format");
		return;
	}
	BOOL connected = ConnectNamedPipe(videoPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
	if (!connected)
	{
		stat.Err(std::format("[{}]: ConnectNamedPipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
		return;
	}
	DWORD nToWrite = desc.Width * desc.Height * 4;
	DWORD nWritten;
	if (!WriteFile(videoPipe, rect.pBits, nToWrite, &nWritten, nullptr) || nWritten != nToWrite)
		stat.Err(std::format("[{}]: WriteFile for pipe failed: {}", __FUNCTION__, ArLastErrorAsString()));
}

void ArFfmpegWriter::StopFfmpeg()
{
	std::reference_wrapper<HANDLE> namedPipes[2] = {videoPipe, audioPipe};
	for (HANDLE& namedPipe : namedPipes)
	{
		if (namedPipe == INVALID_HANDLE_VALUE)
			continue;
		FlushFileBuffers(namedPipe);
		DisconnectNamedPipe(namedPipe);
		CloseHandle(namedPipe);
		namedPipe = INVALID_HANDLE_VALUE;
	}

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

void ArFfmpegWriter::Finish()
{
	StopFfmpeg();
}

ArFfmpegWriter::~ArFfmpegWriter()
{
	StopFfmpeg();
}
