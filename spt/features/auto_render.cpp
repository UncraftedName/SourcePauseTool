#include "stdafx.hpp"

#include "spt\feature.hpp"
#include "spt\utils\signals.hpp"

#include <filesystem>

#undef MAX_VALUE
#include <atlbase.h>
#include <d3d9.h>
#include <shellapi.h>
#include <comdef.h>

void tga_write(const char* filename,
               uint32_t width,
               uint32_t height,
               uint8_t* dataBGRA,
               uint8_t dataChannels = 4,
               uint8_t fileChannels = 3)
{
	FILE* fp = NULL;
	// MSVC prefers fopen_s, but it's not portable
	//fp = fopen(filename, "wb");
	fopen_s(&fp, filename, "wb");
	if (fp == NULL)
		return;

	// You can find details about TGA headers here: http://www.paulbourke.net/dataformats/tga/
	// clang-format off
	uint8_t header[18] = { 0,0,2,0,0,0,0,0,0,0,0,0, (uint8_t)(width%256), (uint8_t)(width/256), (uint8_t)(height%256), (uint8_t)(height/256), (uint8_t)(fileChannels*8), 0x20 };
	// clang-format on
	fwrite(&header, 18, 1, fp);

	std::unique_ptr<uint8_t[]> data = std::make_unique_for_overwrite<uint8_t[]>(width * height * fileChannels);
	size_t x = 0;

	for (uint32_t i = 0; i < width * height; i++)
	{
		for (uint32_t b = 0; b < fileChannels; b++)
		{
			data.get()[x++] = dataBGRA[(i * dataChannels) + (b % dataChannels)];
		}
	}
	fwrite(data.get(), width * height * fileChannels, 1, fp);
	fclose(fp);
}

struct ProfileSlot
{
	CComPtr<IDirect3DSurface9> renderTarget;
	CComPtr<IDirect3DSurface9> offScreenSurf;
	bool hasData = false;
};

enum ProfilerState
{
	PROFILER_STATE_IDLE,
	PROFILER_STATE_QUEUED,
	PROFILER_STATE_RUNNING,
	PROFILER_STATE_STOPPING,
};

class AutoRenderFeature : public FeatureWrapper<AutoRenderFeature>
{
public:
	static inline std::atomic<bool> screenshotQueued = false;

	enum ProfileType
	{
		ARF_PROFILE_SYNCHRONOUS,
		ARF_PROFILE_SYNCHRONOUS_NO_LOCK,
		ARF_PROFILE_ASYNC,
	};

	struct SampleProfile
	{
		static inline std::recursive_mutex mtx;

		struct QueuedProfile
		{
			ProfileType type;
			int nFrames;
			size_t nRenderTargets;
			size_t nOffscreenSurfaces;
		};

		static inline std::atomic<int> nFramesLeft = 0;
		size_t nConsumedFrames = 0;
		static inline std::atomic<size_t> nProcessedFrames = 0;
		std::vector<CComPtr<IDirect3DSurface9>> renderTargets;
		std::vector<CComPtr<IDirect3DSurface9>> offscreenSurfaces;
		std::vector<ProfileSlot> slots;
		ProfileType type;
		size_t consumeTotalMicroSecs = 0;
		std::chrono::high_resolution_clock::time_point startTime;

		QueuedProfile queuedProfile;
		static inline std::atomic<ProfilerState> state = PROFILER_STATE_IDLE;

		bool InitQueued(IDirect3DDevice9* device, std::string& errMsg);
		bool NewFrame(IDirect3DDevice9* device, std::string& errMsg);
		bool ConsumeFrame(IDirect3DDevice9* device, bool finish, std::string& errMsg);
		bool Clear();                                                  // not properly synchronized!!!
		static void DummyJob(void* pBits, size_t seq, UINT w, UINT h); // something to do with the data
	} sampleProfile;

protected:
	virtual void InitHooks() override;
	virtual void LoadFeature() override;
	virtual void UnloadFeature() override;

private:
	size_t nextRenderTarget = 0;

	void OnShaderDevicePresentSignal(IDirect3DDevice9* device);

	static std::optional<std::pair<CComPtr<IDirect3DSurface9>, D3DSURFACE_DESC>> GetBackBufferInfo(
	    IDirect3DDevice9* device,
	    std::string& errMsg);

	static std::vector<CComPtr<IDirect3DSurface9>> CreateRenderTargets(IDirect3DDevice9* device,
	                                                                   size_t nTargets,
	                                                                   UINT w,
	                                                                   UINT h,
	                                                                   std::string& errMsg);
	static std::vector<CComPtr<IDirect3DSurface9>> CreateOffscreenSurfaces(IDirect3DDevice9* device,
	                                                                       size_t nSurfs,
	                                                                       UINT w,
	                                                                       UINT h,
	                                                                       std::string& errMsg);

	static bool StretchToRenderTarget(IDirect3DDevice9* device,
	                                  IDirect3DSurface9* src,
	                                  IDirect3DSurface9* dest,
	                                  std::string& errMsg);

	static bool CaptureScreenshot(IDirect3DDevice9* device, std::string& errMsg);

} static spt_auto_render_feat;

CON_COMMAND_F(spt_ar_screenshot, "Capture screenshot", FCVAR_DONTRECORD)
{
	spt_auto_render_feat.screenshotQueued = true;
}

CON_COMMAND_F(spt_ar_profile,
              "Profile [mode 0-1] [n_frames] [num_render_targets] [num_off_screen_surfs]",
              FCVAR_DONTRECORD)
{
	if (args.ArgC() < 5)
	{
		Warning("Usage: %s\n", spt_ar_profile_command.GetHelpText());
		return;
	}

	std::lock_guard lock(AutoRenderFeature::SampleProfile::mtx);

	spt_auto_render_feat.sampleProfile.queuedProfile = AutoRenderFeature::SampleProfile::QueuedProfile{
	    .type = (AutoRenderFeature::ProfileType)atoi(args.Arg(1)),
	    .nFrames = atoi(args.Arg(2)),
	    .nRenderTargets = (size_t)atoi(args.Arg(3)),
	    .nOffscreenSurfaces = (size_t)atoi(args.Arg(4)),
	};
	spt_auto_render_feat.sampleProfile.state = PROFILER_STATE_QUEUED;

	Msg("Spinning up a new profile...\n");
}

CON_COMMAND_F(spt_ar_profile_stop, "Stop current profile", FCVAR_DONTRECORD)
{
	std::lock_guard lock(AutoRenderFeature::SampleProfile::mtx);
	spt_auto_render_feat.sampleProfile.state = PROFILER_STATE_STOPPING;
	AutoRenderFeature::SampleProfile::nFramesLeft = 0;
}

void AutoRenderFeature::InitHooks() {}

void AutoRenderFeature::LoadFeature()
{
	if (!ShaderDevicePresentSignal.Works)
		return;

	ShaderDevicePresentSignal.Connect(this, &AutoRenderFeature::OnShaderDevicePresentSignal);
	InitCommand(spt_ar_screenshot);
	InitCommand(spt_ar_profile);
	InitCommand(spt_ar_profile_stop);
}

void AutoRenderFeature::UnloadFeature()
{
	sampleProfile.renderTargets.clear();
	sampleProfile.offscreenSurfaces.clear();
}

std::vector<CComPtr<IDirect3DSurface9>> AutoRenderFeature::CreateRenderTargets(IDirect3DDevice9* device,
                                                                               size_t nTargets,
                                                                               UINT w,
                                                                               UINT h,
                                                                               std::string& errMsg)
{
	Assert(nTargets > 0);
	std::vector<CComPtr<IDirect3DSurface9>> ret(nTargets);

	for (auto& rt : ret)
	{
		HRESULT hr =
		    device->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
		if (FAILED(hr))
		{
			errMsg = "CreateRenderTarget failed";
			return {};
		}
	}
	return ret;
}

std::vector<CComPtr<IDirect3DSurface9>> AutoRenderFeature::CreateOffscreenSurfaces(IDirect3DDevice9* device,
                                                                                   size_t nSurfs,
                                                                                   UINT w,
                                                                                   UINT h,
                                                                                   std::string& errMsg)
{
	Assert(nSurfs > 0);
	std::vector<CComPtr<IDirect3DSurface9>> ret(nSurfs);

	for (auto& os : ret)
	{
		HRESULT hr =
		    device->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &os, nullptr);
		if (FAILED(hr))
		{
			errMsg = "CreateOffscreenPlainSurface failed";
			return {};
		}
	}
	return ret;
}

bool AutoRenderFeature::StretchToRenderTarget(IDirect3DDevice9* device,
                                              IDirect3DSurface9* src,
                                              IDirect3DSurface9* dest,
                                              std::string& errMsg)
{
	// TODO allow resizing - can that be done on the GPU?

	HRESULT hr = device->StretchRect(src, nullptr, dest, nullptr, D3DTEXF_NONE);
	if (FAILED(hr))
	{
		errMsg = "StretchRect failed";
		return false;
	}
	return true;
}

bool AutoRenderFeature::CaptureScreenshot(IDirect3DDevice9* device, std::string& errMsg)
{
	auto backBufInfo = GetBackBufferInfo(device, errMsg);
	if (!backBufInfo)
		return false;
	auto& [backBuf, backBufDesc] = *backBufInfo;

	AssertMsg(backBufDesc.Format == D3DFMT_A8R8G8B8,
	          "unexpected back buffer format, handle in StretchToRenderTarget");

	size_t w = backBufDesc.Width;
	size_t h = backBufDesc.Height;

	auto renderTargets = CreateRenderTargets(device, 1, w, h, errMsg);
	if (renderTargets.empty())
		return false;
	auto offscreenSurfaces = CreateOffscreenSurfaces(device, 1, w, h, errMsg);
	if (offscreenSurfaces.empty())
		return false;
	if (!StretchToRenderTarget(device, backBuf, renderTargets[0], errMsg))
		return false;

	HRESULT hr = device->GetRenderTargetData(renderTargets[0], offscreenSurfaces[0]);
	if (FAILED(hr))
	{
		errMsg = "GetRenderTargetData failed";
		return false;
	}

	D3DLOCKED_RECT surfRect;
	hr = offscreenSurfaces[0]->LockRect(&surfRect, nullptr, D3DLOCK_READONLY);
	if (FAILED(hr))
	{
		errMsg = "LockRect failed";
		return false;
	}

	std::filesystem::path tmpFile =
	    std::filesystem::path(std::getenv("UserProfile")) / "Downloads" / "spt_screenshot.tga";
	tga_write(tmpFile.string().c_str(), backBufDesc.Width, backBufDesc.Height, (uint8_t*)surfRect.pBits, 4, 3);
	ShellExecute(nullptr, nullptr, tmpFile.string().c_str(), nullptr, nullptr, SW_SHOW);

	offscreenSurfaces[0]->UnlockRect();

	return true;
}

bool AutoRenderFeature::SampleProfile::InitQueued(IDirect3DDevice9* device, std::string& errMsg)
{
	std::lock_guard lock(mtx);
	if (state != PROFILER_STATE_QUEUED)
		return true;
	Clear();
	this->type = queuedProfile.type;

	auto backBufInfo = GetBackBufferInfo(device, errMsg);
	if (!backBufInfo)
		return false;
	auto& [backBuf, backBufDesc] = *backBufInfo;

	AssertMsg(backBufDesc.Format == D3DFMT_A8R8G8B8,
	          "unexpected back buffer format, handle in StretchToRenderTarget");

	size_t w = backBufDesc.Width;
	size_t h = backBufDesc.Height;

	renderTargets = CreateRenderTargets(device, queuedProfile.nRenderTargets, w, h, errMsg);
	if (renderTargets.empty())
		return false;

	offscreenSurfaces = CreateOffscreenSurfaces(device, queuedProfile.nOffscreenSurfaces, w, h, errMsg);
	if (offscreenSurfaces.empty())
		return false;

	// TODO you can call CreateQuery with a NULL pointer to check for support
	if (type == ARF_PROFILE_ASYNC)
	{
		Assert(queuedProfile.nRenderTargets == queuedProfile.nOffscreenSurfaces);

		slots.resize(queuedProfile.nOffscreenSurfaces);
		for (size_t i = 0; i < queuedProfile.nOffscreenSurfaces; i++)
		{
			auto& slot = slots[i];
			/*HRESULT hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &slot.eventQuery);
			if (FAILED(hr))
			{
				errMsg = "CreateQuery failed";
				return false;
			}*/
			slot.renderTarget = renderTargets[i];
			slot.offScreenSurf = offscreenSurfaces[i];
		}
	}

	// set these up last in case something above fails

	this->nFramesLeft = queuedProfile.nFrames;
	state = PROFILER_STATE_RUNNING;
	startTime = std::chrono::high_resolution_clock::now();

	return true;
}

bool AutoRenderFeature::SampleProfile::NewFrame(IDirect3DDevice9* device, std::string& errMsg)
{
	std::lock_guard lock(SampleProfile::mtx);

	if (state == PROFILER_STATE_QUEUED)
	{
		if (!InitQueued(device, errMsg))
		{
			Warning("failed to initialize profile: %s\n", errMsg.c_str());
			Clear();
		}
	}

	if (state != PROFILER_STATE_IDLE)
	{
		bool finish = nFramesLeft.fetch_sub(1) <= 1;
		if (ConsumeFrame(device, finish, errMsg))
		{
			if (finish)
			{
				auto endTime = std::chrono::high_resolution_clock::now();
				auto totalMillis =
				    std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
				Msg("profile finished for mode %d, consumed %u frames in %.3fs (consume took %.3fms, avg %.3fms/frame)\n",
				    type,
				    nConsumedFrames,
				    totalMillis / 1000.f,
				    consumeTotalMicroSecs / 1000.f,
				    nConsumedFrames > 0 ? consumeTotalMicroSecs / 1000.f / nConsumedFrames : 0);
				Clear();
			}
		}
		else
		{
			Warning("failed to handle profile frame: %s\n", errMsg.c_str());
			Clear();
		}
	}

	return true;
}

bool AutoRenderFeature::SampleProfile::ConsumeFrame(IDirect3DDevice9* device, bool finish, std::string& errMsg)
{
	using clock = std::chrono::steady_clock;
	auto start = clock::now();

	// TODO check resizing
	auto backBufInfo = GetBackBufferInfo(device, errMsg);
	if (!backBufInfo)
		return false;
	auto& [backBuf, backBufDesc] = *backBufInfo;

	switch (type)
	{
	case ARF_PROFILE_SYNCHRONOUS:
	case ARF_PROFILE_SYNCHRONOUS_NO_LOCK:
	{
		// copy to system and wait

		auto& rt = renderTargets[nConsumedFrames % renderTargets.size()];
		if (!StretchToRenderTarget(device, backBuf, rt, errMsg))
			return false;

		nConsumedFrames++;

		auto& os = offscreenSurfaces[nProcessedFrames % offscreenSurfaces.size()];
		HRESULT hr = device->GetRenderTargetData(rt, os);
		if (FAILED(hr))
		{
			errMsg = "GetRenderTargetData failed";
			return false;
		}

		if (type == ARF_PROFILE_SYNCHRONOUS_NO_LOCK)
		{
			D3DLOCKED_RECT surfRect;
			hr = os->LockRect(&surfRect, nullptr, D3DLOCK_READONLY);
			if (FAILED(hr))
			{
				errMsg = "LockRect failed";
				return false;
			}

			DummyJob(surfRect.pBits, nProcessedFrames, backBufDesc.Width, backBufDesc.Height);

			os->UnlockRect();
		}
		nProcessedFrames++;
	}
	break;
	case ARF_PROFILE_ASYNC:
	{
		HRESULT hr;

		// either we have to finish process everything, or lag behind by the number of slots

		// this math is dumb
		while (finish || (int)nProcessedFrames <= (int)nConsumedFrames - (int)slots.size())
		{
			auto& slot = slots[nProcessedFrames % slots.size()];
			if (slot.hasData)
			{
				D3DLOCKED_RECT surfRect;
				hr = slot.offScreenSurf->LockRect(&surfRect, nullptr, D3DLOCK_READONLY);
				if (FAILED(hr))
				{
					errMsg = "LockRect failed";
					return false;
				}

				DummyJob(surfRect.pBits, nProcessedFrames, backBufDesc.Width, backBufDesc.Height);

				slot.offScreenSurf->UnlockRect();

				slot.hasData = false;
				nProcessedFrames++;
			}
			else
			{
				break;
			}
		}

		// queue the next frame

		auto& slot = slots[nConsumedFrames % slots.size()];

		if (!StretchToRenderTarget(device, backBuf, slot.renderTarget, errMsg))
			return false;

		hr = device->GetRenderTargetData(slot.renderTarget, slot.offScreenSurf);
		if (FAILED(hr))
		{
			errMsg = "GetRenderTargetData failed";
			return false;
		}

		slot.hasData = true;
		nConsumedFrames++;

		break;
	}
	default:
		Assert(0);
		errMsg = "unknown profile type";
		return false;
	}

	auto end = clock::now();
	consumeTotalMicroSecs += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

	return true;
}

bool AutoRenderFeature::SampleProfile::Clear()
{
	std::lock_guard lock(mtx);
	AssertMsg(this->nFramesLeft == 0, "a profile is still running");
	nFramesLeft = 0;
	renderTargets.clear();
	offscreenSurfaces.clear();
	slots.clear();
	nConsumedFrames = 0;
	nProcessedFrames = 0;
	consumeTotalMicroSecs = 0;
	state = PROFILER_STATE_IDLE;
	return true;
}

void AutoRenderFeature::SampleProfile::DummyJob(void* pBits, size_t seq, UINT w, UINT h)
{
	/*int numReds = 0;

	for (UINT y = 0; y < h; y++)
	{
		for (UINT x = 0; x < w; x++)
		{
			uint8_t* pixel = (uint8_t*)pBits + (y * w + x) * 4;
			if (pixel[2] > 200 && pixel[1] < 50 && pixel[0] < 50)
				numReds++;
		}
	}

	volatile int sink = numReds;*/

	std::filesystem::path tgaFile = std::filesystem::path(std::getenv("UserProfile")) / "Downloads" / "test"
	                                / std::format("spt_screenshot-{:04d}.tga", seq);

	tga_write(tgaFile.string().c_str(), w, h, (uint8_t*)pBits, 4, 3);
}

void AutoRenderFeature::OnShaderDevicePresentSignal(IDirect3DDevice9* device)
{
	if (screenshotQueued)
	{
		std::string errMsg = "an unspecified error occured";
		if (!CaptureScreenshot(device, errMsg))
			Warning("%s\n", errMsg.c_str());
		screenshotQueued = false;
	}

	std::string errMsg = "an unspecified error occured";
	if (!sampleProfile.NewFrame(device, errMsg))
		Warning("%s\n", errMsg.c_str());
}

std::optional<std::pair<CComPtr<IDirect3DSurface9>, D3DSURFACE_DESC>> AutoRenderFeature::GetBackBufferInfo(
    IDirect3DDevice9* device,
    std::string& errMsg)
{
	CComPtr<IDirect3DSurface9> backBuf;
	// hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuf);
	HRESULT hr = device->GetRenderTarget(0, &backBuf);
	if (FAILED(hr))
	{
		errMsg = "GetRenderTarget failed";
		return {};
	}

	D3DSURFACE_DESC backBufDesc;
	hr = backBuf->GetDesc(&backBufDesc);
	if (FAILED(hr))
	{
		errMsg = "GetDesc failed";
		return {};
	}

	return std::make_pair(backBuf, backBufDesc);
}
