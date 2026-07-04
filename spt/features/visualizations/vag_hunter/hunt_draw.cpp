#include "stdafx.hpp"

#include "hunt.hpp"
#include "spt/utils/ent_utils.hpp"

#define PORTAL_HALF_WIDTH 32.0f
#define PORTAL_HALF_HEIGHT 54.0f

inline ShapeColor BLUE_PORTAL_COLOR{C_OUTLINE(64, 160, 255, 127)};
inline ShapeColor ORANGE_PORTAL_COLOR{C_OUTLINE(255, 160, 32, 127)};

void HtWorker::DrawHistory(MeshRendererDelegate& mr)
{
	if (!StaticMesh::AllValid(meshes.histories))
	{
		meshes.histories.clear();
		meshes.historyBuiltUpToIdx = 0;
	}

	std::unique_lock lk(mtx);

	// build up the history as static mesh(es)
	spt_meshBuilder.CreateMultipleMeshes<StaticMesh>(
	    std::back_inserter(meshes.histories),
	    [this]() { return meshes.historyBuiltUpToIdx < candidateHistory.size(); },
	    [this](MeshBuilderDelegate& mb)
	    {
		    const HtCandidate& c = candidateHistory[meshes.historyBuiltUpToIdx];
		    bool ret = mb.AddCross(c.CalcVagPt(), 5.f, color32(255, 0, 0, 255));
		    if (!ret)
			    return false;
		    if (c.parentIndex != HT_INVALID_CANDIDATE_IDX)
		    {
			    auto& cp = candidateHistory[c.parentIndex];
			    ret = mb.AddLine(c.CalcVagPt(), cp.CalcVagPt(), color32(100, 100, 0, 255));
			    if (!ret)
				    return false;
		    }

		    meshes.historyBuiltUpToIdx++;
		    return true;
	    });

	for (auto& mesh : meshes.histories)
		if (mesh.Valid())
			mr.DrawMesh(mesh);

	// highlight last generation
	mr.DrawMesh(spt_meshBuilder.CreateDynamicMesh(
	    [&](MeshBuilderDelegate& mb)
	    {
		    for (auto cIdx : lastGeneration)
		    {
			    auto& c = candidateHistory[cIdx];
			    mb.AddBox(c.CalcVagPt(), Vector(-5.f), Vector(5.f), vec3_angle, {C_WIRE(0, 255, 0, 255)});
		    }
	    }));

	if (drawPortalsForIdx.has_value())
		DrawDetailedCandidate(mr, *drawPortalsForIdx);
}

void HtWorker::DrawDetailedCandidate(MeshRendererDelegate& mr, candidate_idx cIdx)
{
	Assert(cIdx < candidateHistory.size());

	auto& c = candidateHistory[cIdx];
	for (size_t i = 0; i < meshes.portal.size(); i++)
	{
		if (!meshes.portal[i].Valid())
		{
			meshes.portal[i] = spt_meshBuilder.CreateStaticMesh(
			    [i](MeshBuilderDelegate& mb)
			    {
				    ShapeColor sc = i == HT_ENTRY_BLUE ? BLUE_PORTAL_COLOR : ORANGE_PORTAL_COLOR;
				    sc.wd = WD_BOTH;
				    mb.AddBox(vec3_origin,
				              Vector(-5.f, -PORTAL_HALF_WIDTH, -PORTAL_HALF_HEIGHT),
				              Vector(0.f, PORTAL_HALF_WIDTH, PORTAL_HALF_HEIGHT),
				              vec3_angle,
				              sc);
				    mb.AddTri(Vector(0.f, PORTAL_HALF_WIDTH, PORTAL_HALF_HEIGHT + 3.f),
				              Vector(0.f, -PORTAL_HALF_WIDTH, PORTAL_HALF_HEIGHT + 3.f),
				              Vector(0.f, 0.f, PORTAL_HALF_HEIGHT + 10.f),
				              sc);
				    mb.AddArrow3D(vec3_origin, Vector(1.f, 0.f, 0.f), {6, 20.f, 3.f}, sc);
			    });
		}

		HtPortal p = c.pp.p[i];
		mr.DrawMesh(meshes.portal[i],
		            [p](const CallbackInfoIn& in, CallbackInfoOut& out)
		            { AngleMatrix(QAngle(p.pitch, p.yaw, 0.f), p.pos, out.mat); });
	}

	mr.DrawMesh(spt_meshBuilder.CreateDynamicMesh(
	    [&c](MeshBuilderDelegate& mb)
	    { mb.AddBox(c.CalcVagPt(), Vector(-7.f), Vector(7.f), vec3_angle, {C_OUTLINE(255, 255, 255, 100)}); }));
}

void VagHunterHuntFeature::OnRenderSignal(MeshRendererDelegate& mr)
{
	// draw target(s)

	mr.DrawMesh(spt_meshBuilder.CreateDynamicMesh(
	    [&](MeshBuilderDelegate& mb)
	    {
		    for (auto& pt : vagTarget.pointTarget.points)
			    mb.AddSphere(pt, 10.f, 1, {C_OUTLINE(255, 0, 255, 20)});

		    for (auto& aabb : vagTarget.aabbs)
			    mb.AddBox(vec3_origin, aabb.mins, aabb.maxs, vec3_angle, {C_OUTLINE(255, 255, 0, 20)});

		    if (vagTarget.pendingAabbTargetCorner.has_value())
		    {
			    mb.AddCross(vagTarget.pendingAabbTargetCorner.value(), 10.f, color32{255, 150, 0, 255});
			    mb.AddBox(vec3_origin,
			              vagTarget.pendingAabbTargetCorner.value(),
			              utils::GetPlayerEyePosition(),
			              vec3_angle,
			              {C_OUTLINE(255, 150, 0, 20)});
		    }
	    }));

	if (worker)
		worker->DrawHistory(mr);
}
