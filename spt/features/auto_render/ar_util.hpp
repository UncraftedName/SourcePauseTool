#pragma once

#include "spt/utils/serialize.hpp"

#include "dbg.h"

#include <vector>
#include <d3d9.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

inline std::vector<ComPtr<IDirect3DSurface9>> ArCreateRenderTargets(IDirect3DDevice9* device,
                                                                    D3DFORMAT format,
                                                                    size_t nTargets,
                                                                    UINT w,
                                                                    UINT h,
                                                                    ser::StatusTracker& stat)
{
	if (!stat.Ok())
		return {};

	Assert(nTargets > 0);
	std::vector<ComPtr<IDirect3DSurface9>> ret(nTargets);

	for (auto& rt : ret)
	{
		HRESULT hr = device->CreateRenderTarget(w, h, format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
		if (FAILED(hr))
		{
			stat.Err("[" __FUNCTION__ "]: IDirect3DDevice9::CreateRenderTarget failed");
			return {};
		}
	}
	return ret;
}

inline std::vector<ComPtr<IDirect3DSurface9>> ArCreateOffscreenSurfaces(IDirect3DDevice9* device,
                                                                        D3DFORMAT format,
                                                                        size_t nSurfs,
                                                                        UINT w,
                                                                        UINT h,
                                                                        ser::StatusTracker& stat)
{
	if (!stat.Ok())
		return {};

	Assert(nSurfs > 0);
	std::vector<ComPtr<IDirect3DSurface9>> ret(nSurfs);

	for (auto& os : ret)
	{
		HRESULT hr = device->CreateOffscreenPlainSurface(w, h, format, D3DPOOL_SYSTEMMEM, &os, nullptr);
		if (FAILED(hr))
		{
			stat.Err("[" __FUNCTION__ "]: IDirect3DDevice9::CreateOffscreenPlainSurface failed");
			return {};
		}
	}
	return ret;
}

inline std::pair<ComPtr<IDirect3DSurface9>, D3DSURFACE_DESC> ArGetBackBufferInfo(IDirect3DDevice9* device,
                                                                                 ser::StatusTracker& stat)
{
	if (!stat.Ok())
		return {};

	ComPtr<IDirect3DSurface9> backBuf;
	// hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuf);
	HRESULT hr = device->GetRenderTarget(0, &backBuf);
	if (FAILED(hr))
	{
		stat.Err("[" __FUNCTION__ "]: IDirect3DDevice9::GetRenderTarget failed");
		return {};
	}

	D3DSURFACE_DESC backBufDesc;
	hr = backBuf->GetDesc(&backBufDesc);
	if (FAILED(hr))
	{
		stat.Err("[" __FUNCTION__ "]: IDirect3DSurface9::GetDesc failed");
		return {};
	}

	return std::make_pair(backBuf, backBufDesc);
}

inline void ArStretchToRenderTarget(IDirect3DDevice9* device,
                                    IDirect3DSurface9* src,
                                    IDirect3DSurface9* dest,
                                    ser::StatusTracker& stat)
{
	if (!stat.Ok())
		return;
	// TODO allow resizing - can that be done on the GPU?
	HRESULT hr = device->StretchRect(src, nullptr, dest, nullptr, D3DTEXF_NONE);
	if (FAILED(hr))
		stat.Err("[" __FUNCTION__ "]: IDirect3DDevice9::StretchRect failed");
}
