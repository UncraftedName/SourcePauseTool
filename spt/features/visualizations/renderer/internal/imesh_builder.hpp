#pragma once

#include "mesh_builder_internal.hpp"

template<typename T, typename MapFn>
        requires requires(const T& t, MapFn f) {
	        { f(t) } -> std::same_as<const MbComponentBufs&>;
        }
class MbIMeshBuilder
{
private:
	size_t maxVerts, maxIndices;
	bool dynamic;
	std::span<T> inputComponents;
	MapFn mapFn;

public:
	struct Fused
	{
		IMeshWrapper imw{nullptr, nullptr, true};
		std::span<T> span;

		void Reset()
		{
			*this = {};
		}
	};

private:
	Fused curFused;

public:
	MbIMeshBuilder(std::span<T> components, MapFn mapFn, bool dynamic)
	    : inputComponents{components}, mapFn{std::move(mapFn)}, dynamic{dynamic}

	{
		GetMaxMeshSize(maxVerts, maxIndices, dynamic);
	}

	MbIMeshBuilder(const MbIMeshBuilder&) = delete;

	Fused& GetCurrent()
	{
		Assert(curFused.imw.iMesh);
		return curFused;
	}

private:
	struct FuseData
	{
		std::span<T> span;
		size_t nTotalVerts;
		size_t nTotalIndices;
	};

	void DropInvalidComponents()
	{
		// remove the first component if it's invalid
		while (!inputComponents.empty())
		{
			auto& firstComp = mapFn(inputComponents.front());
			size_t firstNVerts = firstComp.verts.size();
			size_t firstNIndices = firstComp.indices.size();
			if (firstNVerts > maxVerts || firstNIndices > maxIndices)
			{
				AssertMsg(0, "SPT: too many verts/indices");
				inputComponents = inputComponents.subspan(1);
			}
			else if (firstComp.IsEmpty())
			{
				// this is not an error, but it probably indicates a bug in the mesh creation
				AssertMsg(0, "SPT: empty component in" __FUNCTION__);
				inputComponents = inputComponents.subspan(1);
			}
			else
			{
				break;
			}
		}
	}

	FuseData GetNextFuseSpan()
	{
		if (inputComponents.empty())
			return FuseData{};

		/*
		* Find the first component which either exceeds the max size or which has a different
		* primitive/material type.
		*/
		const MbComponentBufs& firstComp = mapFn(inputComponents.front());
		size_t totalNVerts = firstComp.verts.size();
		size_t totalNIndices = firstComp.indices.size();
		size_t nToFuse = 1;
		MeshPrimitiveType primType = firstComp.primType;
		MeshMaterialSimple matType = firstComp.matType;

		for (; nToFuse < inputComponents.size(); nToFuse++)
		{
			const MbComponentBufs& nextComp = mapFn(inputComponents[nToFuse]);
			if (nextComp.primType != primType || nextComp.matType != matType)
				break;
			size_t nextNVerts = totalNVerts + nextComp.verts.size();
			size_t nextNIndices = totalNIndices + nextComp.indices.size();
			if (nextNVerts > maxVerts || nextNIndices > maxIndices)
				break;
			totalNVerts = nextNVerts;
			totalNIndices = nextNIndices;
		}

		auto fuseSpan = inputComponents.first(nToFuse);
		inputComponents = inputComponents.subspan(nToFuse);
		return FuseData{fuseSpan, totalNVerts, totalNIndices};
	}

	void PopulateIMeshBuffers(IMesh* iMesh, const FuseData& fData)
	{
		MeshDesc_t desc;
		iMesh->LockMesh(fData.nTotalVerts, fData.nTotalIndices, desc);

		size_t vertIdx = 0;
		size_t idxIdx = 0; // ;)
		size_t idxOffset = 0;

		for (auto& unmappedComp : fData.span)
		{
			const MbComponentBufs& comp = mapFn(unmappedComp);
			for (const VertexData& vert : comp.verts)
			{
				*(Vector*)((uintptr_t)desc.m_pPosition + vertIdx * desc.m_VertexSize_Position) =
				    vert.pos;
				unsigned char* pColor = desc.m_pColor + vertIdx * desc.m_VertexSize_Color;
				pColor[0] = vert.col.b;
				pColor[1] = vert.col.g;
				pColor[2] = vert.col.r;
				pColor[3] = vert.col.a;
				vertIdx++;
			}
			for (VertIndex vIdx : comp.indices)
				desc.m_pIndices[idxIdx++] = vIdx + desc.m_nFirstVertex + idxOffset;
			idxOffset = vertIdx;
		}
		AssertEquals(vertIdx, fData.nTotalVerts);
		AssertEquals(idxIdx, fData.nTotalIndices);

		iMesh->UnlockMesh(fData.nTotalVerts, fData.nTotalIndices, desc);
	}

	std::optional<IMeshWrapper> CreateAndSetupIMesh(const FuseData& fData)
	{
		if (fData.span.empty())
			return std::nullopt;

		const MbComponentBufs& firstComp = mapFn(fData.span.front());

		CMatRenderContextPtr context{interfaces::materialSystem};
		MaterialRef material = firstComp.GetMaterial();
		context->Bind(material);

		IMesh* iMesh;

		if (dynamic)
		{
			iMesh = context->GetDynamicMesh(true, nullptr, nullptr, material);
		}
		else
		{
			VertexFormat_t vFmt = material->GetVertexFormat();
			if (vFmt == VERTEX_FORMAT_UNKNOWN)
			{
				AssertMsg(0, "We tried so hard, but in the end it doesn't even matter");
				return std::nullopt;
			}
			iMesh = context->CreateStaticMesh(vFmt & ~VERTEX_FORMAT_COMPRESSED,
			                                  TEXTURE_GROUP_STATIC_VERTEX_BUFFER_WORLD,
			                                  material);
		}

		if (!iMesh)
		{
			AssertMsg(0, "SPT: game didn't give us an IMesh* object");
			return std::nullopt;
		}

		switch (firstComp.primType)
		{
		case MeshPrimitiveType::Lines:
			iMesh->SetPrimitiveType(MATERIAL_LINES);
			Assert(fData.nTotalIndices % 2 == 0);
			break;
		case MeshPrimitiveType::Triangles:
			iMesh->SetPrimitiveType(MATERIAL_TRIANGLES);
			Assert(fData.nTotalIndices % 3 == 0);
			break;
		default:
			AssertMsg(0, "Unknown mesh primitive type");
			return std::nullopt;
		}

		return IMeshWrapper{iMesh, std::move(material), dynamic};
	}

public:
	bool FuseNext()
	{
		SPT_VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);

		curFused.Reset();

		DropInvalidComponents();
		FuseData fData = GetNextFuseSpan();
		std::optional<IMeshWrapper> imw = CreateAndSetupIMesh(fData);
		if (!imw)
		{
			curFused.Reset();
			return false;
		}
		PopulateIMeshBuffers(imw->iMesh, fData);

		curFused.imw = std::move(*imw);
		curFused.span = fData.span;
		return true;
	}
};
