#include "stdafx.hpp"

#include "ar_decls.hpp"
#include "ar_util.hpp"

#include <format>

ArSynchronousConsumerManager::ArSynchronousConsumerManager(IDirect3DDevice9* device,
                                                           D3DFORMAT format,
                                                           std::unique_ptr<ArLockableSurfaceConsumer> consumer,
                                                           ser::StatusTracker& stat)
    : consumer(std::move(consumer))
{
	auto [_, desc] = ArGetBackBufferInfo(device, stat);
	if (!stat.Ok())
		return;
	if (format != desc.Format)
	{
		stat.Err(std::format("[{}]: back buffer has format {}, expected {}",
		                     __FUNCTION__,
		                     (int)desc.Format,
		                     (int)format));
		return;
	}

	auto rts = ArCreateRenderTargets(device, format, 1, desc.Width, desc.Height, stat);
	if (!stat.Ok())
		return;
	renderTarget = std::move(rts[0]);

	auto oss = ArCreateOffscreenSurfaces(device, format, 1, desc.Width, desc.Height, stat);
	if (!stat.Ok())
		return;
	offScreenSurface = std::move(oss[0]);
}

void ArSynchronousConsumerManager::Finish(IDirect3DDevice9* device, ser::StatusTracker& stat)
{
	consumer->Finish();
}

void ArSynchronousConsumerManager::NewFrame(IDirect3DDevice9* device, ar_frame_idx frameNum, ser::StatusTracker& stat)
{
	auto [backBuf, backBufDesc] = ArGetBackBufferInfo(device, stat);
	if (!stat.Ok())
		return;

	ArStretchToRenderTarget(device, backBuf.Get(), renderTarget.Get(), stat);
	if (!stat.Ok())
		return;

	HRESULT hr = device->GetRenderTargetData(renderTarget.Get(), offScreenSurface.Get());
	if (FAILED(hr))
	{
		stat.Err("[" __FUNCTION__ "]: IDirect3DDevice9::GetRenderTargetData failed");
		return;
	}

	consumer->LockAndConsume(offScreenSurface.Get(), frameNum, stat);
}

ArAsyncConsumerManager::ArAsyncConsumerManager(IDirect3DDevice9* device,
                                               D3DFORMAT format,
                                               ar_frame_idx nMaxFramesInFlight,
                                               std::unique_ptr<ArLockableSurfaceConsumer> consumer,
                                               ser::StatusTracker& stat)
    : consumer(std::move(consumer)), slots(nMaxFramesInFlight)
{
	if (nMaxFramesInFlight == 0)
	{
		stat.Err(std::format("[{}]: nMaxFramesInFlight must be > 0", __FUNCTION__));
		return;
	}

	auto [_, desc] = ArGetBackBufferInfo(device, stat);
	if (!stat.Ok())
		return;
	if (format != desc.Format)
	{
		stat.Err(std::format("[{}]: back buffer has format {}, expected {}",
		                     __FUNCTION__,
		                     (int)desc.Format,
		                     (int)format));
		return;
	}

	auto rts = ArCreateRenderTargets(device, format, nMaxFramesInFlight, desc.Width, desc.Height, stat);
	auto oss = ArCreateOffscreenSurfaces(device, format, nMaxFramesInFlight, desc.Width, desc.Height, stat);
	if (!stat.Ok())
		return;

	for (ar_frame_idx i = 0; i < nMaxFramesInFlight; i++)
	{
		auto& slot = slots[i];
		slot.renderTarget = std::move(rts[i]);
		slot.offScreenSurf = std::move(oss[i]);
	}
}

void ArAsyncConsumerManager::NewFrame(IDirect3DDevice9* device, ar_frame_idx frameNum, ser::StatusTracker& stat)
{
	if (!stat.Ok())
		return;

	auto& slot = slots[frameNum % slots.size()];

	if (slot.hasData)
	{
		// this slot is full, we have to consume it before we can proceed
		Assert(frameNum == nFramesProcessedByConsumer + slots.size());
		consumer->LockAndConsume(slot.offScreenSurf.Get(), nFramesProcessedByConsumer, stat);
		if (!stat.Ok())
			return;
		slot.hasData = false;
		nFramesProcessedByConsumer++;
	}

	// queue the next frame asynchronously

	auto [backBuf, desc] = ArGetBackBufferInfo(device, stat);
	if (!stat.Ok())
		return;

	ArStretchToRenderTarget(device, backBuf.Get(), slot.renderTarget.Get(), stat);
	if (!stat.Ok())
		return;

	HRESULT hr = device->GetRenderTargetData(slot.renderTarget.Get(), slot.offScreenSurf.Get());
	if (FAILED(hr))
	{
		stat.Err("[" __FUNCTION__ "]: IDirect3DDevice9::GetRenderTargetData failed");
		return;
	}

	slot.hasData = true;
}

void ArAsyncConsumerManager::Finish(IDirect3DDevice9* device, ser::StatusTracker& stat)
{
	for (ar_frame_idx i = nFramesProcessedByConsumer; i < GetNumConsumedFrames(); i++)
	{
		auto& slot = slots[i % slots.size()];
		if (slot.hasData)
		{
			consumer->LockAndConsume(slot.offScreenSurf.Get(), i, stat);
			if (!stat.Ok())
				return;
			slot.hasData = false;
		}
	}
	consumer->Finish();
}

ArThreadedConsumerManager::~ArThreadedConsumerManager()
{
	StopAndJoinWorkerThread();
}

// copy of ArAsyncConsumerManager
ArThreadedConsumerManager::ArThreadedConsumerManager(IDirect3DDevice9* device,
                                                     D3DFORMAT format,
                                                     ar_frame_idx nMaxFramesInFlight,
                                                     std::unique_ptr<ArLockableSurfaceConsumer> consumer,
                                                     ser::StatusTracker& stat)
    : consumer(std::move(consumer)), slots(nMaxFramesInFlight)
{
	if (nMaxFramesInFlight == 0)
	{
		stat.Err(std::format("[{}]: nMaxFramesInFlight must be > 0", __FUNCTION__));
		return;
	}

	auto [_, desc] = ArGetBackBufferInfo(device, stat);
	if (!stat.Ok())
		return;
	if (format != desc.Format)
	{
		stat.Err(std::format("[{}]: back buffer has format {}, expected {}",
		                     __FUNCTION__,
		                     (int)desc.Format,
		                     (int)format));
		return;
	}

	auto rts = ArCreateRenderTargets(device, format, nMaxFramesInFlight, desc.Width, desc.Height, stat);
	auto oss = ArCreateOffscreenSurfaces(device, format, nMaxFramesInFlight, desc.Width, desc.Height, stat);
	if (!stat.Ok())
		return;

	for (ar_frame_idx i = 0; i < nMaxFramesInFlight; i++)
	{
		auto& slot = slots[i];
		slot.renderTarget = std::move(rts[i]);
		slot.offScreenSurf = std::move(oss[i]);
	}

	consumerThread = std::thread(&ArThreadedConsumerManager::WorkerThreadFunc, this);
}

void ArThreadedConsumerManager::WorkerThreadFunc()
{
	for (bool running = true; running;)
	{
		Slot& slot = slots[nFramesProcessedByConsumer % slots.size()];

		{
			std::unique_lock lock(mtx);
			cv.wait(lock, [&slot, this]() { return slot.hasData || finishSignal; });

			// process the rest of the images in the queue before exiting
			if (finishSignal && !slot.hasData)
				return;
		}

		ser::StatusTracker stat;
		consumer->LockAndConsume(slot.offScreenSurf.Get(), nFramesProcessedByConsumer, stat);

		{
			std::lock_guard lock(mtx);

			if (stat.Ok())
			{
				slot.hasData = false;
				++nFramesProcessedByConsumer;
			}
			else
			{
				// have to set this behind the mutex
				consumerStat.Err(std::format("[{}]: {}", __FUNCTION__, stat.GetStatus().errMsg));
				running = false;
			}
		}
		cv.notify_one(); // error or next slot is free
	}
}

void ArThreadedConsumerManager::StopAndJoinWorkerThread()
{
	if (!consumerThread.joinable()) // it's valid to call this even if the thread stops before join
		return;
	mtx.lock();
	finishSignal = true;
	mtx.unlock();
	cv.notify_one();
	consumerThread.join();
}

void ArThreadedConsumerManager::NewFrame(IDirect3DDevice9* device, ar_frame_idx frameNum, ser::StatusTracker& stat)
{
	Slot& slot = slots[frameNum % slots.size()];

	{
		std::unique_lock lock(mtx);
		bool notTimeOut = cv.wait_for(lock,
		                              std::chrono::seconds(10),
		                              [&slot, this]() { return !slot.hasData || !consumerStat.Ok(); });

		if (!consumerStat.Ok())
		{
			stat.Err(std::format("[{}]: error from worker thread: {}",
			                     __FUNCTION__,
			                     consumerStat.GetStatus().errMsg));
			return;
		}

		if (!notTimeOut)
		{
			stat.Err("[" __FUNCTION__ "]: worker thread timed out");
			finishSignal = true;
			cv.notify_one();
			return;
		}
	}

	// queue the next frame asynchronously

	auto [backBuf, desc] = ArGetBackBufferInfo(device, stat);
	if (stat.Ok())
		ArStretchToRenderTarget(device, backBuf.Get(), slot.renderTarget.Get(), stat);

	if (stat.Ok())
	{
		HRESULT hr = device->GetRenderTargetData(slot.renderTarget.Get(), slot.offScreenSurf.Get());
		if (FAILED(hr))
			stat.Err("[" __FUNCTION__ "]: IDirect3DDevice9::GetRenderTargetData failed");
	}

	{
		std::lock_guard lock(mtx);
		if (stat.Ok())
			slot.hasData = true;
		else
			finishSignal = true;
	}
	cv.notify_one(); // error or slot has data
}

void ArThreadedConsumerManager::Finish(IDirect3DDevice9* device, ser::StatusTracker& stat)
{
	StopAndJoinWorkerThread();

	// put this behind the mutex to guarantee memory ordering, maybe unnecessary?
	std::lock_guard lock(mtx);
	if (!consumerStat.Ok())
	{
		stat.Err(
		    std::format("[{}]: error from worker thread: {}", __FUNCTION__, consumerStat.GetStatus().errMsg));
	}

	consumer->Finish();
}
