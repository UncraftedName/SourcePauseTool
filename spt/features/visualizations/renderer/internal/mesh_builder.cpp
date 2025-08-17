#include "stdafx.hpp"

#include <algorithm>

#include "..\mesh_builder.hpp"
#include "mesh_builder_internal.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "interfaces.hpp"
#include "spt\utils\game_detection.hpp"

#include "internal_defs.hpp"
#include "mesh_renderer_internal.hpp"
#include "imesh_builder.hpp"

// TODO use this to implement auto-spilling?
void GetMaxMeshSize(size_t& maxVerts, size_t& maxIndices, bool dynamic)
{
	maxVerts = 32768;
	maxIndices = 32768;

	if (!dynamic)
	{
		if (utils::DoesGameLookLikePortal())
		{
			if (utils::GetBuildNumber() >= 5135)
			{
				maxVerts = 65536;
				maxIndices = 999'999; // unknown upper limit
			}
		}
		// else unkown, assume a safe lower limit
	}

	// make sure value is clapped to the compact mesh limit so we can pack meshes
	maxVerts = std::min({
	    maxVerts,
	    (size_t)std::numeric_limits<VertIndex>::max(),
	    utils::MbCompactMesh::MB_CM_MAX_NUM_VERTS,
	});
}

void MbStagingBufs::PackVertices(std::pmr::memory_resource& mr)
{
	for (auto& component : components)
	{
		if (component.IsEmpty())
			continue;

		utils::MbCompactMesh compactMesh(mr);
		auto& verts = component.verts;
		auto& indices = component.indices;

		switch (component.primType)
		{
		case MeshPrimitiveType::Triangles:
			for (size_t i = 0; i < indices.size(); i += 3)
			{
				[[maybe_unused]] bool ret = compactMesh.AddTriangle(verts[indices[i]],
				                                                    verts[indices[i + 1]],
				                                                    verts[indices[i + 2]]);
				Assert(ret);
			}
			break;
		case MeshPrimitiveType::Lines:
			for (size_t i = 0; i < indices.size(); i += 2)
			{
				[[maybe_unused]] bool ret =
				    compactMesh.AddLine(verts[indices[i]], verts[indices[i + 1]]);
				Assert(ret);
			}
			break;
		default:
			Assert(0);
			break;
		}

		auto& newPoints = compactMesh.GetPoints();
		auto& newColors = compactMesh.GetColors();
		auto& newFaceIndices = compactMesh.GetFaceIndices();
		auto& newLineIndices = compactMesh.GetLineIndices();
		Assert(newPoints.size() == newColors.size());

		if (verts.size() == newPoints.size())
			continue; // not possible to compact

		component.verts.resize(0);

		for (size_t i = 0; i < newPoints.size(); i++)
			component.verts.emplace_back(newPoints[i], newColors[i]);
		if (component.primType == MeshPrimitiveType::Triangles)
			indices.assign(newFaceIndices.cbegin(), newFaceIndices.cend());
		else
			indices.assign(newLineIndices.cbegin(), newLineIndices.cend());

		Assert(indices.size() >= verts.size());
	}
}

MeshPositionInfo MbStagingBufs::CalcPosInfo()
{
	constexpr float inf = std::numeric_limits<float>::infinity();
	MeshPositionInfo pi{Vector{inf}, Vector{-inf}};
	bool any = false;
	for (auto& component : components)
	{
		for (const VertexData& vert : component.verts)
		{
			VectorMin(vert.pos, pi.mins, pi.mins);
			VectorMax(vert.pos, pi.maxs, pi.maxs);
			any = true;
		}
	}

#ifdef DEBUG
	if (any)
	{
		// this isn't strictly necessary, but infinities and NaNs might mess with the system so best to avoid them
		float lower = -VectorMaximum(-pi.mins);
		float upper = VectorMaximum(pi.mins);
		AssertMsg(lower > -1e30 && upper < 1e30, "mesh likely contains weird point(s)");
	}
#endif

	return pi;
}

/**************************************** MESH BUILDER PRO ****************************************/

StaticMesh MeshBuilderPro::CreateStaticMesh(const MeshCreateFunc& createFunc)
{
	SPT_VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);
	std::pmr::monotonic_buffer_resource mr{4096};
	MbStagingBufs stagingBufs{mr, false};
	MeshBuilderDelegate del{stagingBufs};
	createFunc(del);
	stagingBufs.PackVertices(mr);
	MeshPositionInfo posInfo = stagingBufs.CalcPosInfo();
	auto [firstEmpty, _] =
	    std::ranges::remove_if(stagingBufs.components, [](const MbComponentBufs& comp) { return comp.IsEmpty(); });
	std::vector<IMeshWrapper> meshes;
	MbIMeshBuilder builder{std::span{stagingBufs.components.cbegin(), firstEmpty}, std::identity{}, false};
	while (builder.FuseNext())
		meshes.push_back(std::move(builder.GetCurrent().imw));
	meshes.shrink_to_fit();
	return StaticMesh{std::make_shared<StaticMeshUnit>(std::move(meshes), stagingBufs.CalcPosInfo())};
}

DynamicMesh MeshBuilderPro::CreateDynamicMesh(const MeshCreateFunc& createFunc)
{
	SPT_VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);

	if (!spt_meshRenderer.frameData)
	{
		AssertMsg(0, "spt: Frame data was not initialized");
		return {0, -1};
	}
	auto& internalRenderer = spt_meshRenderer.frameData->renderer;
	if (!internalRenderer.acceptUserData)
	{
		AssertMsg(0, "spt: Dynamic meshes can only be created in the MeshRenderSignal!");
		return {0, -1};
	}

	MbParanoidAllocScope scope{};
	auto& mr = spt_meshRenderer.frameData->mr;
	MbStagingBufs stagingBufs{mr, true};
	MeshBuilderDelegate del{stagingBufs};
	createFunc(del);
	MeshPositionInfo posInfo = stagingBufs.CalcPosInfo();
	std::pmr::vector<MbComponentBufs> nonEmptyComponents{&mr};
	for (auto& component : stagingBufs.components)
		if (!component.IsEmpty())
			nonEmptyComponents.push_back(std::move(component));
	auto& dynUnits = spt_meshRenderer.frameData->renderer.dynamicUnits;
	dynUnits.emplace_back(std::move(nonEmptyComponents), posInfo);
	return DynamicMeshToken{dynUnits.size() - 1, spt_meshRenderer.FrameNum()};
}

void MeshBuilderPro::CreateMeshContext(const MeshCreateFunc& createFunc)
{
	std::pmr::monotonic_buffer_resource mr{4096};
	MbStagingBufs stagingBufs{mr, false};
	MeshBuilderDelegate del{stagingBufs};
	createFunc(del);
}

#endif
