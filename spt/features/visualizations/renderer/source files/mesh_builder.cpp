#include "stdafx.h"

#include <algorithm>

#include "..\mesh_builder.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "interfaces.hpp"

#include "..\mesh_defs_private.hpp"

/**************************************** MESH BUILDER INTERNAL ****************************************/

MeshVertData& MeshBuilderInternal::FindOrAddVData(MeshPrimitiveType type, IMaterial* material)
{
	auto vDataIt = std::find_if(curMeshVertData.begin(),
	                            curMeshVertData.end(),
	                            [=](const MeshVertData& vd) { return vd.type == type && vd.material == material; });

	if (vDataIt != curMeshVertData.end())
		return *vDataIt;

	decltype(sharedVertArrays)::iterator vertIt;
	decltype(sharedIndexArrays)::iterator idxIt;

	if (curMeshVertData.size() + 1 > sharedVertArrays.size())
	{
		sharedVertArrays.emplace_back();
		sharedIndexArrays.emplace_back();
		vertIt = --sharedVertArrays.end();
		idxIt = --sharedIndexArrays.end();
	}
	else
	{
		vertIt = sharedVertArrays.begin();
		idxIt = sharedIndexArrays.begin();
		for (size_t _ = 0; _ < curMeshVertData.size(); _++)
		{
			vertIt++;
			idxIt++;
		}
	}
	curMeshVertData.emplace_back(*vertIt, *idxIt, type, material);
	return *--curMeshVertData.end();
}

IMeshWrapper MeshBuilderInternal::CreateIMeshFromVertData(MeshComponentIterator from_,
                                                          MeshComponentIterator to_,
                                                          size_t totalVerts,
                                                          size_t totalIndices,
                                                          bool dynamic)
{
	Assert(totalVerts <= totalIndices);
	Assert(totalVerts <= MAX_MESH_VERTS);
	Assert(totalIndices <= MAX_MESH_INDICES);

	if (totalVerts == 0 || totalIndices == 0)
		return IMeshWrapper{};

	auto from = *from_;

#ifdef DEBUG
	for (auto it_ = from_ + 1; it_ < to_; it_++)
	{
		auto it = *it_;
		AssertEquals(from->type, it->type);
		AssertEquals(from->material, it->material);
		// TODO I want to ensure that meshes with a callback don't get merged
	}
#endif

	if (!from->material)
		return IMeshWrapper{};

	VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESHBUILDER);

	CMatRenderContextPtr context{interfaces::materialSystem};
	context->Bind(from->material);

	if (!dynamic && from->material->GetVertexFormat() == VERTEX_FORMAT_UNKNOWN)
	{
		AssertMsg(0, "We tried so hard, but in the end it doesn't even matter");
		Warning("spt: Static mesh material vertex format is unknown but shouldn't be. Grab a programmer!\n");
		return IMeshWrapper{};
	}

	IMesh* iMesh;
	if (dynamic)
		iMesh = context->GetDynamicMesh(true, nullptr, nullptr, from->material);
	else
		iMesh = context->CreateStaticMesh(from->material->GetVertexFormat() & ~VERTEX_FORMAT_COMPRESSED,
		                                  TEXTURE_GROUP_STATIC_VERTEX_BUFFER_WORLD,
		                                  from->material);

	if (!iMesh)
	{
		AssertMsg(0, "Didn't get an IMesh* object");
		return IMeshWrapper{};
	}

	switch (from->type)
	{
	case MeshPrimitiveType::Lines:
		iMesh->SetPrimitiveType(MATERIAL_LINES);
		Assert(totalIndices % 2 == 0);
		break;
	case MeshPrimitiveType::Triangles:
		iMesh->SetPrimitiveType(MATERIAL_TRIANGLES);
		Assert(totalIndices % 3 == 0);
		break;
	default:
		AssertMsg(0, "Unknown mesh primitive type");
		return IMeshWrapper{};
	}

	// now we can fill the IMesh buffers

	MeshDesc_t desc;
	iMesh->LockMesh(totalVerts, totalIndices, desc);

	size_t vertIdx = 0;
	size_t idxIdx = 0; // ;)
	size_t idxOffset = 0;

	for (MeshComponentIterator it = from_; it < to_; it++)
	{
		for (const VertexData& vert : (**it).verts)
		{
			*(Vector*)((uintptr_t)desc.m_pPosition + vertIdx * desc.m_VertexSize_Position) = vert.pos;
			unsigned char* pColor = desc.m_pColor + vertIdx * desc.m_VertexSize_Color;
			pColor[0] = vert.col.b;
			pColor[1] = vert.col.g;
			pColor[2] = vert.col.r;
			pColor[3] = vert.col.a;
			vertIdx++;
		}
		for (VertIndex vIdx : (**it).indices)
			desc.m_pIndices[idxIdx++] = vIdx + desc.m_nFirstVertex + idxOffset;
		idxOffset = vertIdx; // TODO what does it look like if I remove this lol
	}
	AssertEquals(vertIdx, totalVerts);
	AssertEquals(idxIdx, totalIndices);

	iMesh->UnlockMesh(totalVerts, totalIndices, desc);

	return IMeshWrapper{iMesh, from->material};
}

IMeshWrapper MeshBuilderInternal::CreateIMeshFromVertData(const MeshVertData& vData, bool dynamic)
{
	auto vd = &vData;
	return CreateIMeshFromVertData(&vd, &vd + 1, vData.verts.size(), vData.indices.size(), dynamic);
}

void MeshBuilderInternal::BeginDynamicMeshCreation(MeshComponentIterator from, MeshComponentIterator to, bool dynamic)
{
	creationStatus.from = from;
	creationStatus.to = to;
	creationStatus.dynamic = dynamic;
}

IMeshWrapper MeshBuilderInternal::GetNextIMeshWrapper()
{
	for (;;)
	{
		if (creationStatus.from == creationStatus.to)
			return IMeshWrapper{}; // done with iteration

		if ((**creationStatus.from).verts.size() <= MAX_MESH_VERTS
		    && (**creationStatus.from).indices.size() <= MAX_MESH_INDICES)
		{
			break;
		}
		else
		{
			AssertMsg(0, "too many verts/indices");
			Warning("spt: mesh has too many verts or indices, this would cause a game error\n");
			creationStatus.from++; // skip this component
			continue;
		}
	}
	size_t totalNumVerts = (**creationStatus.from).verts.size();
	size_t totalNumIndices = (**creationStatus.from).indices.size();
	MeshComponentIterator batchEnd = creationStatus.from + 1;
	for (; batchEnd < creationStatus.to; batchEnd++)
	{
		if (totalNumIndices + (**batchEnd).indices.size() > MAX_MESH_INDICES
		    || totalNumVerts + (**batchEnd).verts.size() > MAX_MESH_VERTS)
		{
			break;
		}
		Assert((**batchEnd).indices.size() >= (**batchEnd).verts.size());
		totalNumVerts += (**batchEnd).verts.size();
		totalNumIndices += (**batchEnd).indices.size();
	}
	MeshComponentIterator batchStart = creationStatus.from;
	creationStatus.from = batchEnd;
	creationStatus.lastBatchStart = batchStart;
	creationStatus.lastBatchEnd = batchEnd;
	return CreateIMeshFromVertData(batchStart, batchEnd, totalNumVerts, totalNumIndices, creationStatus.dynamic);
}

void MeshBuilderInternal::FrameCleanup()
{
	// TODO comments!!!

	while (!dynamicMeshUnits.empty())
		dynamicMeshUnits.pop();
#ifdef DEBUG
	for (auto& vertArray : sharedVertArrays)
		Assert(vertArray.size() == 0);
	for (auto& idxArray : sharedIndexArrays)
		Assert(idxArray.size() == 0);
#endif
}

MeshPositionInfo MeshBuilderInternal::_CalcPosInfoForCurrentMesh()
{
	MeshPositionInfo pi{Vector{INFINITY}, Vector{-INFINITY}};
	for (MeshVertData& vData : curMeshVertData)
	{
		for (VertexData& vert : vData.verts)
		{
			VectorMin(vert.pos, pi.mins, pi.mins);
			VectorMax(vert.pos, pi.maxs, pi.maxs);
		}
	}
	for (int i = 0; i < 3; i++)
	{
		Assert(pi.mins[i] > -1e30);
		Assert(pi.maxs[i] < 1e30);
	}
	return pi;
}

const MeshUnit& MeshBuilderInternal::GetDynamicMeshFromToken(DynamicMesh token) const
{
	return dynamicMeshUnits._Get_container()[token.dynamicMeshIdx];
}

/**************************************** MESH BUILDER PRO ****************************************/

StaticMesh MeshBuilderPro::CreateStaticMesh(const MeshCreateFunc& createFunc)
{
	g_meshBuilderInternal.curMeshVertData = {g_meshBuilderInternal.sharedVertDataArrays};
	MeshBuilderDelegate builderDelegate{};
	createFunc(builderDelegate);

	const auto& vDataSlice = g_meshBuilderInternal.curMeshVertData;

	size_t nonEmptyComponents =
	    std::count_if(vDataSlice.begin(),
	                  vDataSlice.end(),
	                  [](MeshVertData& vd) { return vd.verts.size() > 0 && vd.verts.size() > 0; });

	MeshUnit* mu = new MeshUnit{
	    new IMeshWrapper[nonEmptyComponents],
	    nonEmptyComponents,
	    g_meshBuilderInternal._CalcPosInfoForCurrentMesh(),
	};

	int i = 0;
	for (const MeshVertData& vd : vDataSlice)
		if (vd.verts.size() > 0 && vd.verts.size() > 0)
			mu->staticData.meshesArr[i++] = g_meshBuilderInternal.CreateIMeshFromVertData(vd, false);

	g_meshBuilderInternal.curMeshVertData.pop();
	// TODO EMPLACE
	return StaticMesh{mu};
}

DynamicMesh MeshBuilderPro::CreateDynamicMesh(const MeshCreateFunc& createFunc)
{
	VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESHBUILDER);
	g_meshBuilderInternal.curMeshVertData = {g_meshBuilderInternal.sharedVertDataArrays};
	MeshBuilderDelegate builderDelegate{};
	createFunc(builderDelegate);
	MeshPositionInfo posInfo = g_meshBuilderInternal._CalcPosInfoForCurrentMesh();
	g_meshBuilderInternal.dynamicMeshUnits.emplace(g_meshBuilderInternal.curMeshVertData, posInfo);
	return {g_meshBuilderInternal.dynamicMeshUnits.size() - 1, g_meshRenderFrameNum};
}

#endif
