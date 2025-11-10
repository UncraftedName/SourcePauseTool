#pragma once

#include "spt/utils/serialize.hpp"

#include "dbg.h"

#include <vector>
#include <format>

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

inline std::string ArLastErrorAsString()
{
	LPVOID lpMsgBuf = nullptr;
	DWORD dw = GetLastError();

	if (FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	                   NULL,
	                   GetLastError(),
	                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
	                   (LPTSTR)&lpMsgBuf,
	                   0,
	                   NULL)
	    == 0)
	{
		return "FormatMessage failed";
	}
	std::string ret = std::format("({}) {}", dw, (LPCTSTR)lpMsgBuf);
	LocalFree(lpMsgBuf);
	return ret;
}

// TODO can I use std::codecvt_utf8?
inline std::wstring ArUtf8ToUtf16(const char* utf8)
{
	DWORD wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
	std::wstring wstr;
	if (wlen != 0)
	{
		wstr.resize(wlen - 1);
		MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wstr.data(), wlen);
	}
	return wstr;
}

inline std::string ArUtf16ToUtf8(const wchar* utf16)
{
	DWORD len = WideCharToMultiByte(CP_UTF8, 0, utf16, -1, NULL, 0, NULL, NULL);
	std::string str;
	if (len != 0)
	{
		str.resize(len - 1);
		WideCharToMultiByte(CP_UTF8, 0, utf16, -1, str.data(), len, NULL, NULL);
	}
	return str;
}

// TODO go through and put this everywhere

#define AR_STAT_FUNC_ERR_V(stat_, fmt, ...) stat_.Err(std::format("[{}]: " fmt, __FUNCTION__, __VA_ARGS__))
#define AR_STAT_FUNC_ERR(stat_, fmt) AR_STAT_FUNC_ERR_V(stat_, fmt, 0)

#define AR_STAT_FUNC_WIN_ERR_V(stat_, fmt, ...) \
	AR_STAT_FUNC_ERR_V(stat_, fmt ": {}", ArLastErrorAsString(), __VA_ARGS__)
#define AR_STAT_FUNC_WIN_ERR(stat_, fmt) AR_STAT_FUNC_WIN_ERR_V(stat_, fmt, 0)
