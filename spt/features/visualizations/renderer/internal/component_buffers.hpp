#pragma once

#include "internal_defs.hpp"
#include "materials_manager.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "spt/utils/interfaces.hpp"

#include <span>
#include <tuple>
#include <ranges>

using VertexData = utils::MbColoredVert;

struct MbSimpleComponentMap
{
	static inline constexpr size_t N_COMPONENTS = MB_MPT_COUNT * MB_SMMT_COUNT;

	static constexpr size_t GetIndex(MbMeshPrimitiveType primType, MbSimpleMeshMaterialType matType)
	{
		return primType * MB_SMMT_COUNT + matType;
	}

	static constexpr MbMeshPrimitiveType IdxToPrim(size_t idx)
	{
		return static_cast<MbMeshPrimitiveType>(idx / MB_SMMT_COUNT);
	}

	static constexpr MbSimpleMeshMaterialType IdxToMat(size_t idx)
	{
		return static_cast<MbSimpleMeshMaterialType>(idx % MB_SMMT_COUNT);
	}
};

// TODO try using the arena for this
struct MbVertBufs
{
	using vert_type = VertexData;
	using idx_type = VertIndex;

	std::vector<vert_type> verts;
	std::vector<idx_type> indices;

	struct ChunkSize
	{
		size_t nVerts;
		size_t nIndices;
	};

	struct ChunkSpans
	{
		std::span<vert_type> vertSpan;
		std::span<idx_type> idxSpan;
	};

	[[nodiscard]] ChunkSpans AppendChunk(ChunkSize s)
	{
		verts.resize(verts.size() + s.nVerts);
		indices.resize(indices.size() + s.nIndices);
		return ChunkSpans{
		    .vertSpan{std::span(verts).last(s.nVerts)},
		    .idxSpan{std::span{indices}.last(s.nIndices)},
		};
	}

	struct Chunk
	{
		size_t firstVertIdx;
		size_t nVerts;
		size_t firstIdxIdx;
		size_t nIdxs;

		inline ChunkSpans ToSpans(MbVertBufs& bufs) const
		{
			return ChunkSpans{
			    .vertSpan = std::span(bufs.verts).subspan(firstVertIdx, nVerts),
			    .idxSpan = std::span(bufs.indices).subspan(firstIdxIdx, nIdxs),
			};
		}

		bool IsEmpty() const
		{
			return nIdxs == 0;
		}
	};
};

struct MbMeshBufs
{
	std::array<MbVertBufs, MbSimpleComponentMap::N_COMPONENTS> components;
};

struct MbMeshChunks
{
	std::array<MbVertBufs::Chunk, MbSimpleComponentMap::N_COMPONENTS> components;

	MeshPositionInfo CalcPosInfo() const;

	auto Iterate(MbMeshBufs& bufs) const
	{
		return std::views::iota((size_t)0, components.size())
		       | std::views::filter([this](size_t idx) { return !components[idx].IsEmpty(); })
		       | std::views::transform(
		           [this, &bufs](size_t idx)
		           {
			           return std::make_tuple(components[idx].ToSpans(bufs.components[idx]),
			                                  MbSimpleComponentMap::IdxToPrim(idx),
			                                  MbSimpleComponentMap::IdxToMat(idx));
		           });
	}
};

#endif
