#pragma once

#include "internal_defs.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "..\mesh_builder.hpp"

#include <system_error>
#include <d2d1.h>
#include <wincodec.h>

struct MaterialRefMgr
{
	inline void AddRef(IMaterial* mat) const
	{
		if (mat)
			mat->IncrementReferenceCount();
	}

	inline void Release(IMaterial*& mat) const
	{
		if (mat)
		{
			mat->DecrementReferenceCount();
			mat->DeleteIfUnreferenced(); // TODO
			mat = nullptr;
		}
	}
};

using MaterialRef = AutoRefPtr<IMaterial*, MaterialRefMgr>;

#define SPT_TEXTURE_GROUP "spt textures"

#define GLYPH_ATLAS_SIZE 1024
#define MAX_GLYPH_ATLASES 32

struct PackedRect
{
	Rect_t rect; // +x: right, +y: down; x/y in [0, GLYPH_ATLAS_SIZE)
	bool isSideways, wasPacked;
};

struct IRectPacker
{
	virtual void Init() = 0;
	virtual bool TryFit(PackedRect& rect) = 0; // width/height in; x/y/isSideways out
	virtual void TryPack(PackedRect* rects, size_t numRects) = 0;
	virtual void Clear() = 0;
	virtual ~IRectPacker() = 0;
};

// the material manager keeps track of how many of these are initialized
struct GlyphAtlas
{
	Rect_t nextUpdateRect{0, 0, GLYPH_ATLAS_SIZE, GLYPH_ATLAS_SIZE};
	MaterialRef material;
	ITexture* texture; // TODO this should be auto-ref counted

	IWICBitmap* pWicBitMap;
	ID2D1BitmapRenderTarget* pRenderTarget;
	ID2D1SolidColorBrush* pBrush;
	IRectPacker* rectPacker;

	GlyphAtlas() = default;
	GlyphAtlas(const GlyphAtlas&) = delete;
	bool Init();
	void Destroy();

	~GlyphAtlas()
	{
		Destroy();
	}
};
/*
* DWRITE_GLYPH_METRICS (IDWriteFontFace::GetDesignGlyphMetrics) needs glyph indices
* 
* a render target can draw a glyph run, text layout, text format, or geometry
* 
* glyph run:
* - struct, needs glyph indices & a font face
* 
* from my string I can make a IDWriteTextLayout, IDWriteTextAnalyzer::GetGlyphPlacements, IDWriteTextAnalyzer::GetGlyphs
* 
* IDWriteFontFace::GetGlyphIndices is a thing but it is a one-to-one mapping of code points to glyphs
* I probably want IDWriteTextAnalyzer::GetGlyphPlacements or IDWriteTextAnalyzer::GetGlyphs
* 
* https://www.david-colson.com/2020/03/10/exploring-rect-packing.html
* 
* Let's use this:
* https://github.com/nothings/stb/blob/master/stb_rect_pack.h
* don't mark atlases as full, instead keep track of the smallest rectable (by both width & height)
* that doesn't fit.
* 
* system font collection -> font family -> font face; IDWriteFactory::GetSystemFontCollection
* 
* hit testing:
* https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/multimedia/DirectWrite/PadWrites
* 
* I have to enable D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT for color fonts
* 
* what do the numbers mean??? https://learn.microsoft.com/en-us/windows/win32/directwrite/glyphs-and-glyph-runs
*/

struct GlyphKey
{
	IUnknownRef<IDWriteFont3*> font;
	UINT16 glyphIndex;
	UINT32 fontEmSize;
	DWRITE_FONT_WEIGHT weight;
	DWRITE_FONT_STRETCH stretch;
	DWRITE_FONT_STYLE style;

	bool operator==(const GlyphKey& rhs) const
	{
		if (glyphIndex != rhs.glyphIndex || fontEmSize != rhs.fontEmSize || weight != rhs.weight
		    || stretch != rhs.stretch || style != rhs.style)
		{
			return false;
		}
		if (!font && !rhs.font)
			return true; // both null
		if (!font != !rhs.font)
			return false; // one null
		return font->Equals(rhs.font);
	}

	struct Hasher
	{
		// not hashing the font
		size_t operator()(const GlyphKey& k) const
		{
			return std::hash<decltype(k.glyphIndex)>{}(k.glyphIndex)
			       ^ std::hash<decltype(k.fontEmSize)>{}(k.fontEmSize)
			       ^ std::hash<decltype(k.weight)>{}(k.weight) ^ std::hash<decltype(k.stretch)>{}(k.stretch)
			       ^ std::hash<decltype(k.style)>{}(k.style);
		}
	};
};

struct GlyphLocation
{
	size_t atlasIndex;
	PackedRect rect;
};

struct MeshBuilderMatMgr
{
	MaterialRef matOpaque, matAlpha, matAlphaNoZ;

	IDWriteFactory7* pWriteFactory = nullptr;
	ID2D1Factory* pD2DFactory = nullptr;
	IWICImagingFactory* pWicFactory = nullptr;
	IDWriteTextAnalyzer* pTextAnalyzer = nullptr;
	bool dwriteInitSuccess = false;

	std::array<GlyphAtlas, MAX_GLYPH_ATLASES> glyphAtlases{};
	size_t curUsedGlyphAtlases;
	
	std::unordered_map<FontFaceCacheKey, IUnknownRef<IDWriteFontFace3*>, FontFaceCacheKey::Hasher> fontFaceCache;
	std::unordered_map<GlyphKey, GlyphLocation, GlyphKey::Hasher> glyphCache;

	/*
	* Finds associated IMaterials for the given glyphs, packing them into the atlases if they're not cached.
	* [in]  glyphs:       an array of glyphs
	* [in]  glyphMetrics: an array of glyph sizes
	* [out] outMaterials: an array of materials found for each glyph
	* [out] outUvCoords:  an array of rectangles for each glyph in the associated texture of its material
	* [in]  numGlyphs:    the size of these arrays
	*/
	void GetGlyphMaterials(const GlyphKey* glyphs,
	                       const DWRITE_GLYPH_METRICS* glyphMetrics,
	                       MaterialRef* outMaterials,
	                       PackedRect* outUvCoords,
	                       size_t numGlyphs);

	void Load();
	void Unload();
};

inline MeshBuilderMatMgr g_meshMaterialMgr;

#endif
