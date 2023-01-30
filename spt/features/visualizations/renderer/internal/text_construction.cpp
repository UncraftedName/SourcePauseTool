#include "stdafx.h"

#include "internal_defs.hpp"
#include "mesh_builder_internal.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include <locale>
#include <codecvt>

#include "..\mesh_builder.hpp"
#include "spt\utils\math.hpp"

FontInfo MeshBuilderDelegate::FindFont(const FontFaceCacheKey& fontInfo, IDWriteFontCollection2* collection)
{
	if (!g_meshMaterialMgr.initialized)
		return FontInfo{};

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
			return FontInfo{};
		}
	}
	UINT32 index;
	BOOL exists;
	hr = collection->FindFamilyName(fontInfo.familyName.data(), &index, &exists);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "FindFamilyName() error");
		return FontInfo{};
	}
	if (!exists)
	{
		// TODO move to string utils
		std::string str =
		    std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t>{}.to_bytes(fontInfo.familyName);
		Msg("spt: could not find fontPtr family %s\n", str.data());
		return FontInfo{};
	}
	IUnknownRef<IDWriteFontFamily1*> pFontFamily;
	hr = collection->GetFontFamily(index, &pFontFamily._ptr);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "GetFontFamily() error");
		return FontInfo{};
	}
	IUnknownRef<IDWriteFont3*> pFont;
	hr = pFontFamily->GetFirstMatchingFont(fontInfo.weight,
	                                       fontInfo.stretch,
	                                       fontInfo.style,
	                                       (IDWriteFont**)&pFont._ptr);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "GetFirstMatchingFont() error");
		return FontInfo{};
	}
	IUnknownRef<IDWriteFontFace3*> pFontFace;
	hr = pFont->CreateFontFace(&pFontFace._ptr);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "CreateFontFace() error");
		return FontInfo{};
	}
	return g_meshMaterialMgr.fontFaceCache[fontInfo] = FontInfo{std::move(pFont), std::move(pFontFace)};
}

// TODO needs fontPtr size, text scale, wstrlen, color/ztest
void MeshBuilderDelegate::AddText(const wchar_t* wstr, FontInfo fontInfo, const Vector& pos, const QAngle& ang)
{
	if (!wstr || !g_meshMaterialMgr.initialized || !fontInfo.font || !fontInfo.fontFace)
		return;

	int wstrLen = wcslen(wstr);

	if (wstrLen == 0)
		return;

	WindingDir wd = WD_CW;             // TODO
	color32 color{255, 255, 255, 255}; // TODO

	HRESULT hr = S_OK;

	UINT32 guessGlyphCount = 3 * wstrLen / 2 + 16;
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

		hr = g_meshMaterialMgr.pTextAnalyzer->GetGlyphs(wstr,
		                                                wstrLen,
		                                                fontInfo.fontFace,
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

	FLOAT fontEmSize = 30;

	hr = g_meshMaterialMgr.pTextAnalyzer->GetGlyphPlacements(wstr,
	                                                         vClusterMap.data(),
	                                                         vTextProps.data(),
	                                                         wstrLen,
	                                                         vGlyphIndices.data(),
	                                                         vGlyphProps.data(),
	                                                         glyphCount,
	                                                         fontInfo.fontFace,
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

	// fontInfo.fontFace->GetMetrics()

	static std::vector<DWRITE_GLYPH_METRICS> vGlyphMetrics;
	vGlyphMetrics.resize(glyphCount);

	// TODO checkout design glyph advances?
	hr = fontInfo.fontFace->GetDesignGlyphMetrics(vGlyphIndices.data(),
	                                              glyphCount,
	                                              vGlyphMetrics.data(),
	                                              isSideways);

	if (FAILED(hr))
	{
		MsgHResultError(hr, "GetDesignGlyphMetrics() error");
		return;
	}

	GlyphRunInfo runInfo{
	    .fontInfo = fontInfo,
	    .weight = DWRITE_FONT_WEIGHT_NORMAL, // TODO
	    .stretch = DWRITE_FONT_STRETCH_NORMAL,
	    .style = DWRITE_FONT_STYLE_NORMAL,
	    .fontEmSize = fontEmSize,
	    .glyphIndices = vGlyphIndices.data(),
	    .glyphAdvances = vGlyphAdvances.data(),
	    .glyphOffsets = vGlyphOffsets.data(),
	    .glyphMetrics = vGlyphMetrics.data(),
	    .numGlyphs = glyphCount,
	    .isSideways = isSideways,
	    .bidiLevel = 0,
	};
	fontInfo.fontFace->GetMetrics(&runInfo.fontMetrics);

	static std::vector<MaterialRef> vGlyphMaterials;
	static std::vector<PackedRect> vGlyphUvCoords;
	vGlyphMaterials.resize(glyphCount);
	vGlyphUvCoords.resize(glyphCount);

	// TODO the manager should really tell us which glyphs aren't cached maybe
	g_meshMaterialMgr.GetGlyphMaterials(runInfo, vGlyphMaterials.data(), vGlyphUvCoords.data());

	matrix3x4_t matTextToWorld, scaleMat, angMat;
	AngleMatrix(ang, vec3_origin, angMat);
	SetScaleMatrix(1.f / fontEmSize, scaleMat);
	MatrixSetColumn(pos, 3, scaleMat);
	MatrixMultiply(scaleMat, angMat, matTextToWorld);

	const float invFontEmSize = 1.f / fontEmSize;
	float originX = 0;
	float originY = 0;

	for (UINT32 i = 0; i < glyphCount; i++)
	{
		if (vGlyphMaterials[i])
		{
			auto& vdf = GET_VDATA_FACES_CUSTOM_MATERIAL(vGlyphMaterials[i]);
			// this is probably not correct for vertical text TODO not correct right now
			auto& metric = vGlyphMetrics[i];
			// clang-format off
			std::array<Vector, 4> quad{
			    Vector{(originX + metric.leftSideBearing) * invFontEmSize,                        (originY + metric.bottomSideBearing) * invFontEmSize, 0},
			    Vector{(originX + metric.leftSideBearing) * invFontEmSize,                        (originY + metric.topSideBearing), 0},
			    Vector{(originX + metric.advanceWidth + metric.rightSideBearing) * invFontEmSize, (originY + metric.topSideBearing), 0},
			    Vector{(originX + metric.advanceWidth + metric.rightSideBearing) * invFontEmSize, (originY + metric.bottomSideBearing) * invFontEmSize, 0},
			};
			// clang-format on
			for (int k = 0; k < 4; k++)
				utils::VectorTransform(matTextToWorld, quad[k]);

			Assert(!vGlyphUvCoords[i].isSideways); // not handled yet
			Rect_t texRect = vGlyphUvCoords[i].rect;

			// TODO must I flip vertically?
			vdf.verts.emplace_back(quad[0], color, texRect.x, texRect.y);
			vdf.verts.emplace_back(quad[1], color, texRect.x, texRect.y + texRect.height);
			vdf.verts.emplace_back(quad[2], color, texRect.x + texRect.width, texRect.y + texRect.height);
			vdf.verts.emplace_back(quad[3], color, texRect.x + texRect.width, texRect.y);

			_AddFacePolygonIndices(vdf, vdf.verts.size() - 4, 4, wd);
		}
		// TODO check glyph offset instead of advance?
		if (isSideways)
			originX += vGlyphAdvances[i] * (isRightToLeft ? -1 : 1);
		else
			originY += vGlyphAdvances[i]; // is this valid for vertical text? who knows...
	}

	vGlyphMaterials.clear(); // clear to decrement ref count

	// TODO remove me
	std::array<Vector, 4> quad{
	    Vector{pos.x + 50, pos.y - 200, pos.z},
	    Vector{pos.x + 50, pos.y - 300, pos.z},
	    Vector{pos.x - 50, pos.y - 300, pos.z},
	    Vector{pos.x - 50, pos.y - 200, pos.z},
	};
	auto& vdf = GET_VDATA_FACES_CUSTOM_MATERIAL(g_meshMaterialMgr.glyphAtlases[0].material);
	vdf.verts.emplace_back(quad[0], color32{255, 255, 255, 255}, 0, 0);
	vdf.verts.emplace_back(quad[1], color32{255, 255, 255, 255}, 0, 1);
	vdf.verts.emplace_back(quad[2], color32{255, 255, 255, 255}, 1, 1);
	vdf.verts.emplace_back(quad[3], color32{255, 255, 255, 255}, 1, 0);
	_AddFacePolygonIndices(vdf, vdf.verts.size() - 4, 4, wd);
	AddLineStrip(quad.data(), 4, true, {{255, 255, 255, 255}});
}

void MeshBuilderDelegate::AddText(const wchar_t* wstr,
                                  const std::wstring& fontName,
                                  const Vector& pos,
                                  const QAngle& ang)
{
	AddText(wstr, FindFont({fontName}), pos, ang);
}

#endif
