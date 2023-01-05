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
		// convert stb output to what we use
		for (size_t i = 0; i < numRects; i++)
		{
			rects[i].wasPacked = scratch[i].was_packed;
			rects[i].rect.x = scratch[i].x;
			rects[i].rect.y = scratch[i].y;
			rects[i].isSideways = false; // stb doesn't support rotation :/
		}
	}

	virtual void Clear() override {}

	virtual ~StbRectPacker() = default;
};

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

	// dwrite & d2d1 stuff

	dwriteInitSuccess = true;
	HRESULT hr;

	hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory7), (IUnknown**)&pWriteFactory);
	if (FAILED(hr))
	{
		MsgHResultError(hr, "failed to create DWrite factory");
		dwriteInitSuccess = false;
	}

	if (dwriteInitSuccess)
	{
		hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pD2DFactory);
		if (FAILED(hr))
		{
			MsgHResultError(hr, "failed to create D2D1 factory");
			dwriteInitSuccess = false;
		}
	}

	if (dwriteInitSuccess)
	{
		hr = CoCreateInstance(CLSID_WICImagingFactory,
		                      nullptr,
		                      CLSCTX_INPROC_SERVER,
		                      IID_PPV_ARGS(&pWicFactory));
		if (FAILED(hr))
		{
			MsgHResultError(hr, "failed to create WIC factory");
			dwriteInitSuccess = false;
		}
	}

	if (dwriteInitSuccess)
	{
		hr = pWriteFactory->CreateTextAnalyzer(&pTextAnalyzer);
		if (FAILED(hr))
		{
			MsgHResultError(hr, "failed to create text analyzer");
			dwriteInitSuccess = false;
		}
	}
}

void MeshBuilderMatMgr::Unload()
{
	std::array<MaterialRef*, 3> mats{&matOpaque, &matAlpha, &matAlphaNoZ};
	for (MaterialRef* mat : mats)
		*mat = nullptr;

	for (auto& ff : fontFaceCache)
		SafeRelease(ff.second);

	SafeRelease(pTextAnalyzer);
	SafeRelease(pWicFactory);
	SafeRelease(pD2DFactory);
	SafeRelease(pWriteFactory);
}

void MeshBuilderMatMgr::GetGlyphMaterials(const GlyphKey* glyphs,
                                          const DWRITE_GLYPH_METRICS* glyphMetrics,
                                          MaterialRef* outMaterials,
                                          PackedRect* outUvCoords,
                                          size_t numGlyphs)
{
	if (numGlyphs == 0)
		return;

	/*
	* If we pack a glyph into an atlas (or if a glyph is already in one), we want to *immediately* get its material
	* (which will automatically increment the ref count to it) so that we don't lose it before/while rendering.
	* This is because if the current atlases are full, we'll create new ones. The approach is as follows:
	* 
	* We'll have two vectors of unprocessed glyphs (or just their indices in the above arrays) that we swap
	* between as we process new glyphs. We'll iterate over all atlases to try and pack what we can, obtain the
	* material refs where new glyphs were packed to, then spill the leftover glyphs into new atlases.
	*/

	static std::vector<size_t> unprocessedIndices, nextUnprocessedIndices;

	unprocessedIndices.resize(numGlyphs);
	std::iota(unprocessedIndices.begin(), unprocessedIndices.end(), 0);

	for (int mainLoopCount = 0;; mainLoopCount++)
	{
		if (mainLoopCount > 1)
			Warning("spt: recreating glyph atlases more than once, consider turning down your font size");

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
					auto it = glyphCache.find(glyphs[idx]);
					if (it == glyphCache.end())
					{
						nextUnprocessedIndices.push_back(idx);
					}
					else
					{
						GlyphLocation& glyphLocation = it->second;
						outMaterials[idx] = glyphAtlases[glyphLocation.atlasIndex].material;
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

			static std::vector<PackedRect> rectsToPack;
			rectsToPack.resize(unprocessedIndices.size());

			for (size_t i = 0; i < unprocessedIndices.size(); i++)
			{
				size_t idx = unprocessedIndices[i];
				auto& metric = glyphMetrics[idx];
				// TODO height might be wrong
				rectsToPack[i].rect.width =
				    metric.advanceWidth - metric.leftSideBearing - metric.rightSideBearing;
				rectsToPack[i].rect.height =
				    metric.advanceHeight - metric.bottomSideBearing - metric.topSideBearing;
			}

			glyphAtlases[atlasIdx].rectPacker->TryPack(rectsToPack.data(), rectsToPack.size());
			nextUnprocessedIndices.clear();

			for (size_t i = 0; i < unprocessedIndices.size(); i++)
			{
				if (rectsToPack[i].wasPacked)
				{
					size_t idx = unprocessedIndices[i];
					outMaterials[idx] = glyphAtlases[atlasIdx].material;
					outUvCoords[idx] = rectsToPack[i];
					glyphCache[glyphs[idx]] = GlyphLocation{atlasIdx, rectsToPack[i]};
					// TODO update the texture here
				}
				else
				{
					nextUnprocessedIndices.push_back(i);
				}
			}
			if (unprocessedIndices.size() == nextUnprocessedIndices.size())
			{
				if (freshAtlas)
				{
					Warning("spt: couldn't add glyphs to new atlas, is your font size too big?\n");
					Assert(0);
					return;
				}
				checkCache = false; // the unprocessed glyphs are the same, no need to recheck the cache
			}
			else
			{
				checkCache = true;
			}
			unprocessedIndices.swap(nextUnprocessedIndices);
		}
		DevMsg("spt: clearing all glyph atlases\n");
		for (GlyphAtlas& atlas : glyphAtlases)
			atlas.Destroy();
		glyphCache.clear();
		curUsedGlyphAtlases = 0;
	}
}

bool GlyphAtlas::Init()
{
	return false;
}

void GlyphAtlas::Destroy() {}

#endif
