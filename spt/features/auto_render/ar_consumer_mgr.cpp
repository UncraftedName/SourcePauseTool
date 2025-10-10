#include "stdafx.hpp"

#include "ar_jobs.hpp"
#include "ar_util.hpp"

#include <format>

// TODO - write a comment with an explanation of how the audio pipe blocks after a few frames

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

short* ArSynchronousConsumerManager::LockSoundBuf(size_t nSamplePairs, ser::StatusTracker& stat)
{
	consumeMtx.lock();
	soundBuf.resize(nSamplePairs * 2);
	return soundBuf.data();
}

void ArSynchronousConsumerManager::UnlockSoundBuf(ser::StatusTracker& stat)
{
	consumer->ConsumeAudio(soundBuf.data(), soundBuf.size() / 2, stat);
	consumeMtx.unlock();
}

void ArSynchronousConsumerManager::Finish(ser::StatusTracker& stat)
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

	std::lock_guard lk(consumeMtx);
	consumer->LockAndConsume(offScreenSurface.Get(), frameNum, stat);
}

ArThreadedConsumerManager::~ArThreadedConsumerManager()
{
	StopAndJoinWorkers();
}

// copy of ArAsyncConsumerManager
ArThreadedConsumerManager::ArThreadedConsumerManager(IDirect3DDevice9* device,
                                                     D3DFORMAT format,
                                                     ar_frame_idx nMaxFramesInFlight,
                                                     std::unique_ptr<ArLockableSurfaceConsumer> consumer,
                                                     ser::StatusTracker& stat)
    : consumer(std::move(consumer)), videoSlots(nMaxFramesInFlight), audioSlots(nMaxFramesInFlight)
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
		auto& slot = videoSlots[i];
		slot.renderTarget = std::move(rts[i]);
		slot.offScreenSurf = std::move(oss[i]);
	}

	workers[AR_THRD_VIDEO].thread = std::thread(&ArThreadedConsumerManager::VideoThreadFunc, this);
	workers[AR_THRD_AUDIO].thread = std::thread(&ArThreadedConsumerManager::AudioThreadFunc, this);
}

short* ArThreadedConsumerManager::LockSoundBuf(size_t nSamplePairs, ser::StatusTracker& stat)
{
	AudioSlot& slot = audioSlots[nAudioSlotsProduced % audioSlots.size()];

	bool workerThreadErr = false;
	{
		std::unique_lock lock(mtx);
		// wait for free slot or error
		cv.wait(lock,
		        [&slot, &workerThreadErr, this]()
		        {
			        workerThreadErr =
			            !workers[AR_THRD_VIDEO].stat.Ok() || !workers[AR_THRD_AUDIO].stat.Ok();
			        return !slot.hasData || workerThreadErr || finishSignal;
		        });
	}

	// the actual errors are reported from the render thread
	if (workerThreadErr)
	{
		stat.Err(std::format("[{}]: worker thread error", __FUNCTION__));
		return nullptr;
	}

	slot.samples.resize(nSamplePairs * 2);
	return slot.samples.data();
}

void ArThreadedConsumerManager::UnlockSoundBuf(ser::StatusTracker& stat)
{
	AudioSlot& slot = audioSlots[nAudioSlotsProduced % audioSlots.size()];
	{
		std::lock_guard lock(mtx);
		slot.hasData = true;
	}
	cv.notify_all();
	++nAudioSlotsProduced;
}

void ArThreadedConsumerManager::VideoThreadFunc()
{
	for (bool running = true; running;)
	{
		VideoSlot& slot = videoSlots[nFramesProcessedByConsumer % videoSlots.size()];

		{
			std::unique_lock lock(mtx);
			cv.wait(lock, [&slot, this]() { return slot.hasData || finishSignal; });

			// process the rest of the slots
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
				workers[AR_THRD_VIDEO].stat.Concat(std::move(stat));
				running = false;
			}
		}
		cv.notify_all(); // error or next slot is free
	}
}

// exactly the same as the video thread
void ArThreadedConsumerManager::AudioThreadFunc()
{
	for (bool running = true; running;)
	{
		AudioSlot& slot = audioSlots[nAudioSlotsProcessedByConsumer % audioSlots.size()];

		{
			std::unique_lock lock(mtx);
			cv.wait(lock, [&slot, this]() { return slot.hasData || finishSignal; });

			// process the rest of the slots
			if (finishSignal && !slot.hasData)
				return;
		}

		ser::StatusTracker stat;
		consumer->ConsumeAudio(slot.samples.data(), slot.samples.size() / 2, stat);

		{
			std::lock_guard lock(mtx);

			if (stat.Ok())
			{
				slot.hasData = false;
				++nAudioSlotsProcessedByConsumer;
			}
			else
			{
				// have to set this behind the mutex
				workers[AR_THRD_AUDIO].stat.Concat(std::move(stat));
				running = false;
			}
		}
		cv.notify_all(); // error or next slot is free
	}
}

void ArThreadedConsumerManager::StopAndJoinWorkers()
{
	mtx.lock();
	finishSignal = true;
	mtx.unlock();
	cv.notify_all();
	for (auto& worker : workers)
		if (worker.thread.joinable())
			worker.thread.join();
}

void ArThreadedConsumerManager::NewFrame(IDirect3DDevice9* device, ar_frame_idx frameNum, ser::StatusTracker& stat)
{
	VideoSlot& slot = videoSlots[frameNum % videoSlots.size()];

	bool notTimedOut = true;
	bool workerThreadErr = false;

	{
		std::unique_lock lock(mtx);
#if 0
		notTimedOut = cv.wait_for(lock,
		                          std::chrono::seconds(10),
		                          [&slot, &workerThreadErr, this]()
		                          {
			                          workerThreadErr = !workers[AR_THRD_VIDEO].stat.Ok()
			                                            || !workers[AR_THRD_AUDIO].stat.Ok();
			                          return !slot.hasData || workerThreadErr;
		                          });
#else
		cv.wait(lock,
		        [&slot, &workerThreadErr, this]()
		        {
			        workerThreadErr =
			            !workers[AR_THRD_VIDEO].stat.Ok() || !workers[AR_THRD_AUDIO].stat.Ok();
			        return !slot.hasData || workerThreadErr;
		        });
#endif
		finishSignal |= workerThreadErr; // stop audio producer
	}

	if (workerThreadErr)
	{
		StopAndJoinWorkers();
		stat.Concat(std::move(workers[AR_THRD_VIDEO].stat));
		stat.Concat(std::move(workers[AR_THRD_VIDEO].stat));
		return;
	}

	if (!notTimedOut)
	{
		stat.Err("[" __FUNCTION__ "]: worker thread timed out");
		StopAndJoinWorkers();
		return;
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
	cv.notify_all(); // error or slot has data
}

void ArThreadedConsumerManager::Finish(ser::StatusTracker& stat)
{
	StopAndJoinWorkers();

	// put this behind the mutex to guarantee memory ordering, maybe unnecessary?
	std::lock_guard lock(mtx);
	stat.Concat(std::move(workers[AR_THRD_VIDEO].stat));
	stat.Concat(std::move(workers[AR_THRD_AUDIO].stat));
	consumer->Finish();
}
