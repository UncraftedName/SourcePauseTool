#pragma once

#include "internal_defs.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "..\mesh_builder.hpp"
#include "materialsystem\itexture.h"

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

struct TextureRefMgr
{
	inline void AddRef(ITexture* tex) const
	{
		if (tex)
			tex->IncrementReferenceCount();
	}

	inline void Release(ITexture*& tex) const
	{
		if (tex)
			tex->DecrementReferenceCount(); // TODO delete if unreferenced
		tex = nullptr;
	}
};

using MaterialRef = AutoRefPtr<IMaterial*, MaterialRefMgr>;
using TextureRef = AutoRefPtr<ITexture*, TextureRefMgr>;

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
	virtual void TryPack(PackedRect* rects, size_t numRects) = 0; // width/height in; x/y/isSideways out
	virtual ~IRectPacker(){};
};

struct GlyphAtlas;

class GlyphTextureRegenerator : public ITextureRegenerator
{
	IUnknownRef<IWICBitmap*> pWicBitMap;

public:
	GlyphTextureRegenerator(const GlyphAtlas* atlas);
	virtual void RegenerateTextureBits(ITexture* pTexture, IVTFTexture* pVTFTexture, Rect_t* pRect) override;
	virtual void Release() override;
};

// passed to the material manager and atlases
struct GlyphRunInfo
{
	FontInfo fontInfo;
	DWRITE_FONT_WEIGHT weight;
	DWRITE_FONT_STRETCH stretch;
	DWRITE_FONT_STYLE style;
	FLOAT fontEmSize;
	const UINT16* glyphIndices;
	const FLOAT* glyphAdvances;
	const DWRITE_GLYPH_OFFSET* glyphOffsets;
	const DWRITE_GLYPH_METRICS* glyphMetrics;
	size_t numGlyphs;
	BOOL isSideways;
	UINT32 bidiLevel; // odd: RTL, even: LTR
};

struct GlyphAtlas
{
	Rect_t nextUpdateRect;
	MaterialRef material;
	TextureRef texture;
	GlyphTextureRegenerator* textureRegenerator = nullptr;

	IUnknownRef<IWICBitmap*> pWicBitMap;
	ID2D1BitmapRenderTarget* pRenderTarget;
	ID2D1SolidColorBrush* pBrush;

	IRectPacker* rectPacker = nullptr;

	bool initialized = false;

	GlyphAtlas() = default;
	GlyphAtlas(const GlyphAtlas&) = delete;
	void Init();
	void Destroy();

	void DrawGlyphs(const GlyphRunInfo& glyphRunInfo,
	                const size_t* arrayIndices,
	                const PackedRect* rects,
	                size_t numArrayIndices);
	void UpdateTexture();
	void GrowUpdateRect(const PackedRect& packedRect);

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
		Assert(font && rhs.font);
		return glyphIndex == rhs.glyphIndex && fontEmSize == rhs.fontEmSize && weight == rhs.weight
		       && stretch == rhs.stretch && style == rhs.style && font->Equals(rhs.font);
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
	GlyphAtlas* atlas;
	PackedRect rect;
};

struct MeshBuilderMatMgr
{
	MaterialRef matOpaque, matAlpha, matAlphaNoZ;

	IDWriteFactory7* pWriteFactory = nullptr;
	ID2D1Factory* pD2DFactory = nullptr;
	IWICImagingFactory* pWicFactory = nullptr;
	IDWriteTextAnalyzer* pTextAnalyzer = nullptr;
	bool initialized = false;

	std::array<GlyphAtlas, MAX_GLYPH_ATLASES> glyphAtlases{};
	size_t curUsedGlyphAtlases = 0;
	int recreateAtlasCount = 0;

	std::unordered_map<FontFaceCacheKey, FontInfo, FontFaceCacheKey::Hasher> fontFaceCache;
	std::unordered_map<GlyphKey, GlyphLocation, GlyphKey::Hasher> glyphCache;

	bool texturesNeedUpdate = false;

	/*
	* Finds associated IMaterials for the given glyphs, packing them into the atlases if they're not cached.
	* [in]  glyphs:       an array of glyphs
	* [in]  glyphMetrics: an array of glyph sizes
	* [out] outMaterials: an array of materials found for each glyph
	* [out] outUvCoords:  an array of rectangles for each glyph in the associated texture of its material
	* [in]  numGlyphs:    the size of these arrays
	*/
	void GetGlyphMaterials(const GlyphRunInfo& glyphRunInfo, MaterialRef* outMaterials, PackedRect* outUvCoords);

	void UpdateTextures();

	void Load();
	void Unload();
};

inline MeshBuilderMatMgr g_meshMaterialMgr;

#endif
