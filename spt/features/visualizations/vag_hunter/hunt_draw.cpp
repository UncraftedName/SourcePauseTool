#include "stdafx.hpp"

#include "hunt.hpp"
#include "spt/utils/ent_utils.hpp"

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

	// draw all candidates

	if (worker)
	{
		std::unique_lock kl(worker->mtx);

		mr.DrawMesh(spt_meshBuilder.CreateDynamicMesh(
		    [&](MeshBuilderDelegate& mb)
		    {
			    for (auto& c : worker->candidateHistory)
			    {
				    mb.AddCross(c.pp.CalcVagPt((HtPortalColor)c.entryColor),
				                5.f,
				                color32(255, 0, 0, 255));
				    if (c.parentIndex != HT_INVALID_CANDIDATE_IDX)
				    {
					    auto& cp = worker->candidateHistory[c.parentIndex];
					    mb.AddLine(c.pp.CalcVagPt((HtPortalColor)c.entryColor),
					               cp.pp.CalcVagPt((HtPortalColor)cp.entryColor),
					               color32(100, 100, 0, 255));
				    }
			    }

			    for (auto cIdx : worker->lastGeneration)
			    {
				    auto& c = worker->candidateHistory[cIdx];
				    mb.AddBox(c.pp.CalcVagPt((HtPortalColor)c.entryColor),
				              Vector(-5.f),
				              Vector(5.f),
				              vec3_angle,
				              {C_WIRE(0, 255, 0, 255)});
			    }
		    }));
	}
}
