#pragma once

#include "spt\feature.hpp"
#include "spt\utils\signals.hpp"
#include "spt\utils\serialize.hpp"

#include <filesystem>

#include <d3d9.h>
#include <shellapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

using ar_frame_idx = size_t;

/*
* The thing which eats the off screen surfaces and does stuff with them NOM NOM NOM NOM.
* Depending on the mode, this might be run on a dedicated thread, but should probably always run synchronously.
*/
class ArLockableSurfaceConsumer
{
public:
	ArLockableSurfaceConsumer() = default;
	ArLockableSurfaceConsumer(ArLockableSurfaceConsumer&) = delete;
	ArLockableSurfaceConsumer(ArLockableSurfaceConsumer&&) = delete;
	virtual ~ArLockableSurfaceConsumer() {}

	void LockAndConsume(IDirect3DSurface9* offScreenSurface, ar_frame_idx idx, ser::StatusTracker& stat);
	virtual void Finish() {};

protected:
	virtual void Consume(D3DLOCKED_RECT rect,
	                     const D3DSURFACE_DESC& desc,
	                     ar_frame_idx idx,
	                     ser::StatusTracker& stat) = 0;
};

class ArNullConsumer : public ArLockableSurfaceConsumer
{
protected:
	virtual void Consume(D3DLOCKED_RECT rect,
	                     const D3DSURFACE_DESC& desc,
	                     ar_frame_idx idx,
	                     ser::StatusTracker& stat) override {};
};

// writes out TGA files to e.g. C:\Users\<user>\Downloads\spt_screenshot-{:04d}.tga
class ArTgaWriter : public ArLockableSurfaceConsumer
{
	std::string fmt;
	bool singleFrame;
	bool openAfterWrite;
	bool hasWritten = false;

public:
	explicit ArTgaWriter(std::string fmt, bool singleFrame, bool openAfterWrite)
	    : fmt(std::move(fmt)), singleFrame(singleFrame), openAfterWrite(openAfterWrite)
	{
		Assert(!openAfterWrite || singleFrame);
	}

protected:
	virtual void Consume(D3DLOCKED_RECT rect,
	                     const D3DSURFACE_DESC& desc,
	                     ar_frame_idx idx,
	                     ser::StatusTracker& stat) override;
};

class ArFfmpegWriter : public ArLockableSurfaceConsumer
{
	HANDLE videoPipe = INVALID_HANDLE_VALUE;
	HANDLE audioPipe = INVALID_HANDLE_VALUE;
	HANDLE jobObject = NULL;
	bool procValid = false;
	PROCESS_INFORMATION ffmpegProc{};
	std::optional<DWORD>& ffmpegReturnCode;

public:
	struct InitArgs
	{
		const std::wstring ffmpegWorkingDir;
		std::wstring cmd;                 // fully formatted with no remaining substitutions
		const std::wstring videoPipeName; // can be null
		const std::wstring audioPipeName; // can be null
		size_t width, height;             // used as an estimation for pipe buffer size
	};

	explicit ArFfmpegWriter(InitArgs& args, std::optional<DWORD>& ffmpegReturnCode, ser::StatusTracker& stat);
	virtual ~ArFfmpegWriter();

	virtual void Finish();

protected:
	virtual void Consume(D3DLOCKED_RECT rect,
	                     const D3DSURFACE_DESC& desc,
	                     ar_frame_idx idx,
	                     ser::StatusTracker& stat) override;

private:
	void StopFfmpeg();
};

/*
* Gets called on the render thread when a device is presented. Distributes jobs to the consumer
* synchronously or asynchronously depending on the implementation.
*/
class ArSyncManager
{
	ar_frame_idx nConsumedFrames = 0;

public:
	ArSyncManager() = default;
	ArSyncManager(ArSyncManager&) = delete;
	ArSyncManager(ArSyncManager&&) = delete;
	virtual ~ArSyncManager() {};

protected:
	virtual void NewFrame(IDirect3DDevice9* device, size_t frameNum, ser::StatusTracker& stat) = 0;

public:
	virtual void Finish(IDirect3DDevice9* device, ser::StatusTracker& stat) = 0;

	void OnDevicePresent(IDirect3DDevice9* device, ser::StatusTracker& stat)
	{
		NewFrame(device, nConsumedFrames++, stat);
	}

	auto GetNumConsumedFrames() const
	{
		return nConsumedFrames;
	}
};

// calls LockAndConsume synchronously on the render thread
class ArSynchronousConsumerManager : public ArSyncManager
{
	std::unique_ptr<ArLockableSurfaceConsumer> consumer;
	ComPtr<IDirect3DSurface9> renderTarget;
	ComPtr<IDirect3DSurface9> offScreenSurface;

public:
	explicit ArSynchronousConsumerManager(IDirect3DDevice9* device,
	                                      D3DFORMAT format,
	                                      std::unique_ptr<ArLockableSurfaceConsumer> consumer,
	                                      ser::StatusTracker& stat);

	virtual void NewFrame(IDirect3DDevice9* device, size_t frameNum, ser::StatusTracker& stat) override;
	virtual void Finish(IDirect3DDevice9* device, ser::StatusTracker& stat) override;
};

// keeps multiple slots of render target + offscreen surface pairs in flight, but still consumes on the render thread
class ArAsyncConsumerManager : public ArSyncManager
{
	struct Slot
	{
		ComPtr<IDirect3DSurface9> renderTarget;
		ComPtr<IDirect3DSurface9> offScreenSurf;
		bool hasData = false;
	};

	std::unique_ptr<ArLockableSurfaceConsumer> consumer;
	std::vector<Slot> slots;
	ar_frame_idx nFramesProcessedByConsumer = 0;

public:
	explicit ArAsyncConsumerManager(IDirect3DDevice9* device,
	                                D3DFORMAT format,
	                                ar_frame_idx nMaxFramesInFlight,
	                                std::unique_ptr<ArLockableSurfaceConsumer> consumer,
	                                ser::StatusTracker& stat);

	virtual void NewFrame(IDirect3DDevice9* device, ar_frame_idx frameNum, ser::StatusTracker& stat) override;
	virtual void Finish(IDirect3DDevice9* device, ser::StatusTracker& stat) override;
};

// producer/consumer with the consumer on a separate thread
class ArThreadedConsumerManager : public ArSyncManager
{
	struct Slot
	{
		ComPtr<IDirect3DSurface9> renderTarget;
		ComPtr<IDirect3DSurface9> offScreenSurf;
		bool hasData = false;
	};

	std::mutex mtx;
	std::condition_variable cv;
	std::thread consumerThread;
	ser::StatusTracker consumerStat;
	bool finishSignal = false;

	std::unique_ptr<ArLockableSurfaceConsumer> consumer;
	std::vector<Slot> slots;
	ar_frame_idx nFramesProcessedByConsumer = 0;

	void WorkerThreadFunc();
	void StopAndJoinWorkerThread();

public:
	virtual ~ArThreadedConsumerManager();
	explicit ArThreadedConsumerManager(IDirect3DDevice9* device,
	                                   D3DFORMAT format,
	                                   ar_frame_idx nMaxFramesInFlight,
	                                   std::unique_ptr<ArLockableSurfaceConsumer> consumer,
	                                   ser::StatusTracker& stat);

	virtual void NewFrame(IDirect3DDevice9* device, ar_frame_idx frameNum, ser::StatusTracker& stat) override;
	virtual void Finish(IDirect3DDevice9* device, ser::StatusTracker& stat) override;
};
