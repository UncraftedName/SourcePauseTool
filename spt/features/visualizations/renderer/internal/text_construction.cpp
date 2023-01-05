#include "stdafx.h"

#include "internal_defs.hpp"
#include "mesh_builder_internal.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include <locale>
#include <codecvt>
#include "..\mesh_builder.hpp"

IUnknownRef<IDWriteFontFace3*> MeshBuilderDelegate::FindFont(const FontFaceCacheKey& fontInfo,
                                                             IDWriteFontCollection2* collection)
{
	if (!g_meshMaterialMgr.dwriteInitSuccess)
		return nullptr;

	auto it = g_meshMaterialMgr.fontFaceCache.find(fontInfo);
	if (it != g_meshMaterialMgr.fontFaceCache.end())
		return it->second;

	HRESULT hr;
	if (!collection)
	{
		// not sure if TYPOGRAPHIC is the best option here
		// TODO could I use this to download fonts??
		hr = g_meshMaterialMgr.pWriteFactory->GetSystemFontCollection(false,
		                                                              DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC,
		                                                              &collection);
		if (FAILED(hr))
		{
			MsgHResultError(hr, "GetSystemFontCollection() error");
			return nullptr;
		}
	}
	UINT32 index;
	BOOL exists;
	hr = collection->FindFamilyName(fontInfo.familyName.data(), &index, &exists);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "FindFamilyName() error");
		return nullptr;
	}
	if (!exists)
	{
		// TODO move to string utils
		std::string str =
		    std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t>{}.to_bytes(fontInfo.familyName);
		Msg("spt: could not find font family %s\n", str.data());
		return nullptr;
	}
	IDWriteFontFamily1* fontFamily;
	hr = collection->GetFontFamily(index, &fontFamily);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "GetFontFamily() error");
		return nullptr;
	}
	IDWriteFont3* font = nullptr;
	hr = fontFamily->GetFirstMatchingFont(fontInfo.weight, fontInfo.stretch, fontInfo.style, (IDWriteFont**)&font);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "GetFirstMatchingFont() error");
		return nullptr;
	}
	IDWriteFontFace3* fontFace;
	hr = font->CreateFontFace(&fontFace);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "CreateFontFace() error");
		return nullptr;
	}
	// don't create a ref to the same ptr, use its copy ctor instead
	IUnknownRef<IDWriteFontFace3*> ret = fontFace;
	g_meshMaterialMgr.fontFaceCache[fontInfo] = ret;
	return ret;
}

// TODO needs font size, text scale, wstrlen, color/ztest
void MeshBuilderDelegate::AddText(const wchar_t* wstr,
                                  IDWriteFontFace3* pFontFace,
                                  const Vector& pos,
                                  const QAngle& ang)
{
	auto& mgr = g_meshMaterialMgr;

	if (!wstr || !mgr.dwriteInitSuccess || !pFontFace)
		return;

	int wstrLen = wcslen(wstr);

	if (wstrLen == 0)
		return;

	HRESULT hr = S_OK;

	UINT32 guessGlyphCount = wstrLen; // windows recommends '3 * wstrLen / 2 + 16' but we'll probably use english
	UINT32 glyphCount = 0;

	DWRITE_SCRIPT_ANALYSIS scriptAnalysis{}; // TODO

	BOOL isSideways = true;
	BOOL isRightToLeft = false;
	const wchar_t* localName = L"en-us";

	IDWriteNumberSubstitution* pNumberSubstitution = NULL;
	const DWRITE_TYPOGRAPHIC_FEATURES** pFeatures = NULL;
	const UINT32* pFeatureRangeLengths = NULL;
	UINT32 numFeatureRanges = 0;

	static std::vector<UINT16> vClusterMap;
	static std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> vTextProps;
	static std::vector<UINT16> vGlyphIndices;
	static std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> vGlyphProps;

	const int maxTrycCount = 3;
	for (int tryCount = 0; tryCount < maxTrycCount; tryCount++)
	{
		vClusterMap.resize(guessGlyphCount);
		vTextProps.resize(guessGlyphCount);
		vGlyphIndices.resize(guessGlyphCount);
		vGlyphProps.resize(guessGlyphCount);

		hr = mgr.pTextAnalyzer->GetGlyphs(wstr,
		                                  wstrLen,
		                                  pFontFace,
		                                  isSideways,
		                                  isRightToLeft,
		                                  &scriptAnalysis,
		                                  localName,
		                                  pNumberSubstitution,
		                                  pFeatures,
		                                  pFeatureRangeLengths,
		                                  numFeatureRanges,
		                                  guessGlyphCount,
		                                  vClusterMap.data(),
		                                  vTextProps.data(),
		                                  vGlyphIndices.data(),
		                                  vGlyphProps.data(),
		                                  &glyphCount);

		if (SUCCEEDED(hr))
		{
			break;
		}
		else if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))
		{
			guessGlyphCount *= 2;
		}
		else
		{
			MsgHResultError(hr, "GetGlyphs() error");
			return;
		}
	}

	if (FAILED(hr))
	{
		Msg("spt: GetGlyphs() failed %d times\n", maxTrycCount);
		return;
	}

	static std::vector<FLOAT> vGlyphAdvances;
	static std::vector<DWRITE_GLYPH_OFFSET> vGlyphOffsets;
	vGlyphAdvances.resize(glyphCount);
	vGlyphOffsets.resize(glyphCount);

	FLOAT fontEmSize = 96;

	hr = mgr.pTextAnalyzer->GetGlyphPlacements(wstr,
	                                           vClusterMap.data(),
	                                           vTextProps.data(),
	                                           wstrLen,
	                                           vGlyphIndices.data(),
	                                           vGlyphProps.data(),
	                                           glyphCount,
	                                           pFontFace,
	                                           fontEmSize,
	                                           isSideways,
	                                           isRightToLeft,
	                                           &scriptAnalysis,
	                                           localName,
	                                           pFeatures,
	                                           pFeatureRangeLengths,
	                                           numFeatureRanges,
	                                           vGlyphAdvances.data(),
	                                           vGlyphOffsets.data());

	if (FAILED(hr))
	{
		MsgHResultError(hr, "GetGlyphPlacements() error");
		return;
	}

	static std::vector<DWRITE_GLYPH_METRICS> vGlyphMetrics;
	vGlyphMetrics.resize(glyphCount);

	hr = pFontFace->GetDesignGlyphMetrics(vGlyphIndices.data(), glyphCount, vGlyphMetrics.data(), isSideways);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "GetDesignGlyphMetrics() error");
		return;
	}

	static std::vector<MaterialRef> vGlyphMaterials;
	static std::vector<PackedRect> vGlyphUvCoords;
	vGlyphMaterials.resize(glyphCount);
	vGlyphUvCoords.resize(glyphCount);

	vGlyphMaterials.clear(); // clear to decrement ref count

	for (UINT32 i = 0; i < glyphCount; i++)
	{
	}
}

void MeshBuilderDelegate::AddText(const wchar_t* wstr,
                                  const std::wstring& fontName,
                                  const Vector& pos,
                                  const QAngle& ang)
{
	AddText(wstr, FindFont({fontName}), pos, ang);
}

#endif
