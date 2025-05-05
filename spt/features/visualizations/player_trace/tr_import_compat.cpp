#include <stdafx.hpp>

#include "tr_import_export.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

using namespace player_trace;

template<typename T, tr_struct_version STRUCT_VERSION = TR_LUMP_VERSION(T), typename... LUMP_DEPENDENCIES>
struct TrLumpCompatHandlerFrom : TrLumpCompatHandler
{
	TrLumpCompatHandlerFrom()
	    : TrLumpCompatHandler{TR_LUMP_NAME(T), STRUCT_VERSION, {TR_LUMP_NAME(LUMP_DEPENDENCIES)...}}
	{
	}
};

struct : public TrLumpCompatHandlerFrom<TrPlayerData_v1, TR_LUMP_VERSION(TrPlayerData_v1), Vector, QAngle, TrTransform>
{
	virtual bool HandleCompat(ITrReader& rd, TrLump& lump, std::vector<std::byte>& dataInOut) const
	{
		if (dataInOut.size() % sizeof(TrPlayerData_v1) != 0)
			return false;

		auto pd1Sp = std::span{(TrPlayerData_v1*)dataInOut.data(), dataInOut.size() / sizeof(TrPlayerData_v1)};
		std::vector<TrPlayerData_v2> pd2Vec;

		auto convertVecIdx = [](TrIdx<Vector> idx)
		{ return idx.IsValid() && **idx == Vector{NAN} ? TrIdx<Vector>{} : idx; };

		auto convertTransIdx = [](TrIdx<TrTransform> idx)
		{
			if (idx.IsValid() && **idx->posIdx == Vector{NAN} && **idx->angIdx == QAngle{NAN, NAN, NAN})
				return TrIdx<TrTransform>{};
			return idx;
		};

		for (auto& pd1 : pd1Sp)
		{
			TrPlayerData_v2 pd2{
			    .tick = pd1.tick,
			    .qPosIdx = convertVecIdx(pd1.qPosIdx),
			    .qVelIdx = convertVecIdx(pd1.qVelIdx),
			    ._unused{},
			    .vVelIdx{},
			    .transEyesIdx = convertTransIdx(pd1.transEyesIdx),
			    .transSgEyesIdx = convertTransIdx(pd1.transSgEyesIdx),
			    .transVPhysIdx = convertTransIdx(pd1.transVPhysIdx),
			    .contactPtsSp = pd1.contactPtsSp,
			    .m_fFlags = pd1.m_fFlags,
			    .fov = pd1.fov,
			    .m_iHealth = pd1.m_iHealth,
			    .m_lifeState = pd1.m_lifeState,
			    .m_CollisionGroup = pd1.m_CollisionGroup,
			    .m_MoveType = pd1.m_MoveType,
			};
			pd2Vec.push_back(pd2);
		}
		size_t nBytes = pd2Vec.size() * sizeof(pd2Vec[0]);
		dataInOut.resize(nBytes);
		memcpy(dataInOut.data(), pd2Vec.data(), nBytes);
		lump.struct_version = TR_LUMP_VERSION(TrPlayerData_v2);
		return true;
	}
} static _player_data_1_2_handler;

struct : public TrLumpCompatHandlerFrom<Vector, 0>
{
	virtual bool HandleCompat(ITrReader& rd, TrLump& lump, std::vector<std::byte>& dataInOut) const
	{
		if (dataInOut.size() % sizeof(Vector) != 0)
			return false;
		auto vecSpan = std::span{(Vector*)dataInOut.data(), dataInOut.size() / sizeof(Vector)};
		for (Vector& v : vecSpan)
			if (v == vec3_invalid)
				v = Vector{NAN, NAN, NAN};
		lump.struct_version = 1;
		return true;
	}
} static _vec_0_1_handler;

#endif
