#include "stdafx.h"

#include "internal_defs.hpp"
#include "materials_manager.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include <numeric>

#define STB_RECT_PACK_IMPLEMENTATION
#include "thirdparty\stb_rect_pack.h"

#include "interfaces.hpp"

#pragma comment(lib, "Dwrite")
#pragma comment(lib, "ole32")
#pragma comment(lib, "D2d1")

/**************************************** RECT PACKER ****************************************/

class StbRectPacker : public IRectPacker
{
	stbrp_context ctx;
	std::array<stbrp_node, GLYPH_ATLAS_SIZE> nodes;

public:
	virtual void Init() override
	{
		stbrp_init_target(&ctx, GLYPH_ATLAS_SIZE, GLYPH_ATLAS_SIZE, nodes.data(), nodes.size());
	}

	virtual void TryPack(PackedRect* rects, size_t numRects) override
	{
		// convert our input to what stb uses
		static std::vector<stbrp_rect> scratch;
		scratch.resize(numRects);
		for (size_t i = 0; i < numRects; i++)
		{
			scratch[i].w = rects->rect.width;
			scratch[i].h = rects->rect.height;
			// no current use for ID field, could point back to glyph info but I don't think that's needed
		}
		stbrp_pack_rects(&ctx, scratch.data(), numRects);
		AssertMsg(ctx.free_head, "spt: stb rect packer out of memory"); // TODO I think I should keep track
		// convert stb output to what we use
		for (size_t i = 0; i < numRects; i++)
		{
			rects[i].wasPacked = scratch[i].was_packed;
			rects[i].rect.x = scratch[i].x;
			rects[i].rect.y = scratch[i].y;
			rects[i].isSideways = false; // stb doesn't support rotation :/
		}
	}

	virtual ~StbRectPacker() = default;
};

/**************************************** TEXTURE REGENERATOR ****************************************/

GlyphTextureRegenerator::GlyphTextureRegenerator(const GlyphAtlas* atlas) : pWicBitMap(atlas->pWicBitMap) {}

void GlyphTextureRegenerator::RegenerateTextureBits(ITexture*, IVTFTexture* pVTFTexture, Rect_t* pRect)
{
	if (pVTFTexture->Format() != IMAGE_FORMAT_BGRA8888)
		Error("spt: Invalid texture format in %s()", __FUNCTION__);

	HRESULT hr;
#ifdef DEBUG
	WICPixelFormatGUID pxlFmt;
	hr = pWicBitMap->GetPixelFormat(&pxlFmt);
	Assert(SUCCEEDED(hr) && pxlFmt == GUID_WICPixelFormat8bppAlpha);
#endif

	// TODO actually only update the specified rect

	static std::array<uint8_t, GLYPH_ATLAS_SIZE * GLYPH_ATLAS_SIZE> buf;
	hr = pWicBitMap->CopyPixels(nullptr, GLYPH_ATLAS_SIZE, sizeof buf, buf.data());
	uint8_t* textureBuf = pVTFTexture->ImageData();
	for (int i = 0; i < sizeof buf; i++)
	{
		textureBuf[i * 4 + 0] = 255;
		textureBuf[i * 4 + 1] = 0;
		textureBuf[i * 4 + 2] = 0;
		textureBuf[i * 4 + 3] = buf[i];
	}
}

void GlyphTextureRegenerator::Release()
{
	pWicBitMap.Release();
	delete this;
}

/**************************************** MATERIAL MANAGER ****************************************/

void MeshBuilderMatMgr::Load()
{
	KeyValues* kv;

	kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	matOpaque = interfaces::materialSystem->CreateMaterial("_spt_UnlitOpaque", kv);

	kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	matAlpha = interfaces::materialSystem->CreateMaterial("_spt_UnlitTranslucent", kv);

	kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	kv->SetInt("$ignorez", 1);
	matAlphaNoZ = interfaces::materialSystem->CreateMaterial("_spt_UnlitTranslucentNoZ", kv);

	// factories!!!

	initialized = true;
	HRESULT hr;

	hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory7), (IUnknown**)&pWriteFactory);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "DWriteCreateFactory() error");
		initialized = false;
		return;
	}
	hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pD2DFactory);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "D2D1CreateFactory() error");
		initialized = false;
		return;
	}
	hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWicFactory));
	if (FAILED(hr))
	{
		MsgHResultError(hr, "CoCreateInstance() error");
		initialized = false;
		return;
	}
	hr = pWriteFactory->CreateTextAnalyzer(&pTextAnalyzer);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "CreateTextAnalyzer() error");
		initialized = false;
		return;
	}
}

void MeshBuilderMatMgr::Unload()
{
	std::array<MaterialRef*, 3> mats{&matOpaque, &matAlpha, &matAlphaNoZ};
	for (MaterialRef* mat : mats)
		(*mat).Release();

	fontFaceCache.clear();

	SafeRelease(pTextAnalyzer);
	SafeRelease(pWicFactory);
	SafeRelease(pD2DFactory);
	SafeRelease(pWriteFactory);
}

void MeshBuilderMatMgr::GetGlyphMaterials(const GlyphRunInfo& glyphRunInfo,
                                          MaterialRef* outMaterials,
                                          PackedRect* outUvCoords)
{
	if (glyphRunInfo.numGlyphs == 0 || !initialized)
		return;

	/*
	* If we pack a glyph into an atlas (or if a glyph is already in one), we want to *immediately* get its material
	* (which will automatically increment the ref count to it) so that we don't lose it before/while rendering.
	* This is because if the current atlases are full, we'll create new ones. The approach is as follows:
	* 
	* We'll have two vectors of unprocessed indices of the above arrys that we swap between as we process new
	* glyphs. For each unprocessed glyph, we'll check if we've packed it already. If not, we'll pack what we can
	* into the first available atlas, get the material ref for it, and spill glyphs into the next atlases. If we
	* can't insert into any atlas, then we destroy all of them and start fresh.
	*/

	static std::vector<GlyphKey> cacheKeys;
	cacheKeys.resize(glyphRunInfo.numGlyphs);
	for (size_t i = 0; i < glyphRunInfo.numGlyphs; i++)
	{
		cacheKeys[i].font = glyphRunInfo.fontInfo.font;
		cacheKeys[i].glyphIndex = glyphRunInfo.glyphIndices[i];
		cacheKeys[i].fontEmSize = glyphRunInfo.fontEmSize;
		cacheKeys[i].weight = glyphRunInfo.weight;
		cacheKeys[i].stretch = glyphRunInfo.stretch;
		cacheKeys[i].style = glyphRunInfo.style;
	}

	static std::vector<size_t> unprocessedIndices, nextUnprocessedIndices;

	unprocessedIndices.resize(glyphRunInfo.numGlyphs);
	std::iota(unprocessedIndices.begin(), unprocessedIndices.end(), 0);

	for (int mainLoopCount = 0;; mainLoopCount++)
	{
		bool checkCache = !glyphCache.empty();

		for (size_t atlasIdx = 0; atlasIdx < MAX_GLYPH_ATLASES; atlasIdx++)
		{
			if (unprocessedIndices.empty())
				return; // no more left

			if (checkCache)
			{
				unprocessedIndices.clear();
				for (size_t idx : unprocessedIndices)
				{
					auto it = glyphCache.find(cacheKeys[idx]);
					if (it == glyphCache.end())
					{
						nextUnprocessedIndices.push_back(idx);
					}
					else
					{
						GlyphLocation& glyphLocation = it->second;
						outMaterials[idx] = glyphLocation.atlas->material;
						outUvCoords[idx] = glyphLocation.rect;
					}
				}
				if (nextUnprocessedIndices.empty())
					return; // remaining glyphs were cached

				unprocessedIndices.swap(nextUnprocessedIndices);
			}

			bool freshAtlas = false;
			if (atlasIdx >= curUsedGlyphAtlases)
			{
				DevMsg(2, "spt: creating a new glyph atlas\n");
				glyphAtlases[atlasIdx].Init();
				++curUsedGlyphAtlases;
				freshAtlas = true;
			}

			// TODO this loop here makes this whole thing O(a*n) but we don't care about order

			static std::vector<PackedRect> glyphRects;
			glyphRects.resize(unprocessedIndices.size());

			for (size_t i = 0; i < unprocessedIndices.size(); i++)
			{
				size_t idx = unprocessedIndices[i];
				auto& metric = glyphRunInfo.glyphMetrics[idx];
				// TODO height might be wrong
				glyphRects[i].rect.width =
				    metric.advanceWidth - metric.leftSideBearing - metric.rightSideBearing;
				glyphRects[i].rect.height =
				    metric.advanceHeight - metric.bottomSideBearing - metric.topSideBearing;
			}

			glyphAtlases[atlasIdx].rectPacker->TryPack(glyphRects.data(), glyphRects.size());
			nextUnprocessedIndices.clear();

			static std::vector<size_t> indicesForAtlas;
			indicesForAtlas.clear();

			for (size_t i = 0; i < unprocessedIndices.size(); i++)
			{
				size_t idx = unprocessedIndices[i];
				if (glyphRects[i].wasPacked)
				{
					outMaterials[idx] = glyphAtlases[atlasIdx].material;
					outUvCoords[idx] = glyphRects[i];
					glyphCache[cacheKeys[idx]] =
					    GlyphLocation{&glyphAtlases[atlasIdx], glyphRects[i]};
					indicesForAtlas.push_back(idx);
				}
				else
				{
					nextUnprocessedIndices.push_back(idx);
				}
			}

			// TODO
			/*glyphAtlases[atlasIdx].DrawGlyphs(glyphRunInfo,
			                                  indicesForAtlas.data(),
			                                  glyphRects.data(),
			                                  indicesForAtlas.size());*/

			if (unprocessedIndices.size() == nextUnprocessedIndices.size())
			{
				if (freshAtlas)
				{
					Warning("spt: couldn't add glyphs to new atlas, is your font size too big?\n");
					// Assert(0); TODO eee
					return;
				}
				checkCache = false; // the unprocessed glyphs are the same, no need to recheck the cache
			}
			else
			{
				checkCache = true;
				unprocessedIndices.swap(nextUnprocessedIndices);
			}
		}
		DevMsg("spt: recreating all glyph atlases\n");
		if (mainLoopCount > 0)
			Warning("spt: recreating glyph atlases more than once, consider turning down your font size\n");
		for (GlyphAtlas& atlas : glyphAtlases)
			atlas.Destroy();
		glyphCache.clear();
		curUsedGlyphAtlases = 0;
		++recreateAtlasCount;
	}
}

void MeshBuilderMatMgr::UpdateTextures()
{
	if (!texturesNeedUpdate || !initialized)
		return;
	for (size_t i = 0; i < curUsedGlyphAtlases; i++)
		glyphAtlases[i].UpdateTexture();
	texturesNeedUpdate = false;
}

/**************************************** GLYPH ATLAS ****************************************/

void GlyphAtlas::Init()
{
	if (!g_meshMaterialMgr.initialized)
	{
		initialized = false;
		return;
	}

	char materialName[64];
	char textureName[64];

	snprintf(textureName,
	         sizeof textureName,
	         "_spt_GlyphAtlas_%i_%i_%d",
	         g_meshMaterialMgr.recreateAtlasCount,
	         g_meshMaterialMgr.curUsedGlyphAtlases,
	         (int)time(nullptr));

	texture = interfaces::materialSystem->CreateProceduralTexture(textureName,
	                                                              SPT_TEXTURE_GROUP,
	                                                              GLYPH_ATLAS_SIZE,
	                                                              GLYPH_ATLAS_SIZE,
	                                                              IMAGE_FORMAT_BGRA8888,
	                                                              TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT
	                                                                  | TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD
	                                                                  | TEXTUREFLAGS_SINGLECOPY);

	snprintf(materialName,
	         sizeof materialName,
	         "_spt_UnlitTranslucentText_%i_%i_%d",
	         g_meshMaterialMgr.recreateAtlasCount,
	         g_meshMaterialMgr.curUsedGlyphAtlases,
	         (int)time(nullptr));

	KeyValues* kv = new KeyValues("unlitgeneric");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	kv->SetString("$basetexture", textureName);

	material = interfaces::materialSystem->CreateMaterial(materialName, kv);

	nextUpdateRect = Rect_t{0, 0, 0, 0};

	HRESULT hr;
	initialized = true;

	IWICBitmap* rawBitMapPtr;

	hr = g_meshMaterialMgr.pWicFactory->CreateBitmap(GLYPH_ATLAS_SIZE,
	                                                 GLYPH_ATLAS_SIZE,
	                                                 GUID_WICPixelFormat8bppAlpha,
	                                                 WICBitmapCacheOnDemand,
	                                                 &rawBitMapPtr);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "CreateBitmap() error");
		initialized = false;
		return;
	}
	pWicBitMap = rawBitMapPtr;

	// now that we've created the bitmap, we can create our texture regenerator (which will read from the bitmap)

	textureRegenerator = new GlyphTextureRegenerator{this};

	// TODO
	typedef void(__thiscall * _SetTextureRegenerator)(ITexture * thisptr, ITextureRegenerator*);
	((_SetTextureRegenerator**)(ITexture*)texture)[0][0x30 / 4](texture, textureRegenerator);

	nextUpdateRect = Rect_t{0, 0, GLYPH_ATLAS_SIZE, GLYPH_ATLAS_SIZE};

	hr = g_meshMaterialMgr.pD2DFactory
	         ->CreateWicBitmapRenderTarget(pWicBitMap,
	                                       D2D1_RENDER_TARGET_PROPERTIES{D2D1_RENDER_TARGET_TYPE_DEFAULT,
	                                                                     D2D1_PIXEL_FORMAT{DXGI_FORMAT_UNKNOWN,
	                                                                                       D2D1_ALPHA_MODE_UNKNOWN},
	                                                                     0.f,
	                                                                     0.f,
	                                                                     D2D1_RENDER_TARGET_USAGE_NONE,
	                                                                     D2D1_FEATURE_LEVEL_DEFAULT},
	                                       (ID2D1RenderTarget**)&pRenderTarget);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "CreateWicBitmapRenderTarget() error");
		initialized = false;
		return;
	}
	// TODO CreateBitmapBrush?
	hr = pRenderTarget->CreateSolidColorBrush(D2D1_COLOR_F{255, 255, 255, 255}, &pBrush);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "CreateSolidColorBrush() error");
		initialized = false;
		return;
	}

	// TODO this doesn't show :////
	/*pRenderTarget->BeginDraw();
	pRenderTarget->Clear({255, 255, 255, 0});
	pRenderTarget->DrawEllipse(D2D1_ELLIPSE{D2D1_POINT_2F{200, 300}, 70, 50}, pBrush, 5);
	pRenderTarget->EndDraw();
	nextUpdateRect = Rect_t{0, 0, GLYPH_ATLAS_SIZE, GLYPH_ATLAS_SIZE};
	g_meshMaterialMgr.texturesNeedUpdate = true;*/

	rectPacker = new StbRectPacker{};
	rectPacker->Init();
}

void GlyphAtlas::Destroy()
{
	initialized = false;
	delete rectPacker;
	SafeRelease(pBrush);
	SafeRelease(pRenderTarget);
	pWicBitMap->Release();
	if (!texture && textureRegenerator)
		textureRegenerator->Release(); // the texture won't call regenerator->Release() for us
	textureRegenerator = nullptr;
	texture.Release();
	material.Release();
}

void GlyphAtlas::DrawGlyphs(const GlyphRunInfo& glyphRunInfo,
                            const size_t* arrayIndices,
                            const PackedRect* packedRects,
                            size_t numArrayIndices)
{
	if (!initialized)
		return;

	pRenderTarget->BeginDraw();

	// to draw glyphs where we want in the atlas, we'll draw multiple "runs" with only 1 character
	// TODO change this so that we batch this all together in one run by setting the glyphOffsets
	DWRITE_GLYPH_RUN glyphRun{
	    .fontFace = glyphRunInfo.fontInfo.fontFace,
	    .fontEmSize = glyphRunInfo.fontEmSize,
	    .glyphCount = 1,
	    .isSideways = glyphRunInfo.isSideways,
	    .bidiLevel = glyphRunInfo.bidiLevel,
	};

	for (size_t i = 0; i < numArrayIndices; i++)
	{
		Assert(!packedRects[i].isSideways); // not handled yet
		Rect_t rect = packedRects[i].rect;
		if (rect.width == 0 || rect.height == 0)
			continue;

		// TODO I think my transform needs to be relative to the baseline...
		// pRenderTarget->SetTransform(D2D1::Matrix3x2F::Translation(rect.x, rect.y));
		size_t curIdx = arrayIndices[i];
		glyphRun.glyphIndices = glyphRunInfo.glyphIndices + curIdx;
		glyphRun.glyphAdvances = glyphRunInfo.glyphAdvances + curIdx;
		glyphRun.glyphOffsets = glyphRunInfo.glyphOffsets + curIdx;

		// it's important that the glyphs are snapped to the nearest integer pixel in the atlas
		pRenderTarget->DrawGlyphRun(D2D1_POINT_2F(rect.x, rect.y), &glyphRun, pBrush);

		GrowUpdateRect(packedRects[i]);
	}

	HRESULT hr = pRenderTarget->EndDraw();
	if (FAILED(hr))
		MsgHResultError(hr, __FUNCTION__ "() error");
}

void GlyphAtlas::UpdateTexture()
{
	if (!initialized || nextUpdateRect.width == 0 || nextUpdateRect.height == 0)
		return;
	// TODO
	typedef void(__thiscall * _Download)(ITexture * thisptr, Rect_t*, int);
	((_Download**)(ITexture*)texture)[0][0x34 / 4](texture, &nextUpdateRect, 0);
	nextUpdateRect.width = nextUpdateRect.height = 0;
}

void GlyphAtlas::GrowUpdateRect(const PackedRect& packedRect)
{
	Assert(!packedRect.isSideways); // not handled yet
	Assert(packedRect.rect.x >= 0 && packedRect.rect.y >= 0 && packedRect.rect.width >= 0
	       && packedRect.rect.height >= 0);
	if (packedRect.rect.width == 0 || packedRect.rect.height == 0)
		return;

	if (nextUpdateRect.height == 0 || nextUpdateRect.width == 0)
	{
		nextUpdateRect = packedRect.rect;
	}
	else
	{
		int xMax = MAX(nextUpdateRect.x + nextUpdateRect.width, packedRect.rect.x + packedRect.rect.width);
		int yMax = MAX(nextUpdateRect.y + nextUpdateRect.height, packedRect.rect.y + packedRect.rect.height);
		nextUpdateRect.x = MIN(nextUpdateRect.x, packedRect.rect.x);
		nextUpdateRect.y = MIN(nextUpdateRect.y, packedRect.rect.y);
		nextUpdateRect.width = xMax - nextUpdateRect.x;
		nextUpdateRect.height = yMax - nextUpdateRect.y;
	}
	g_meshMaterialMgr.texturesNeedUpdate = true;
}

#endif
