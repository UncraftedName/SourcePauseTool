#include "stdafx.hpp"

#include "hunt.hpp"
#include "spt/utils/ent_utils.hpp"

void VagHunterHuntFeature::OnRenderSignal(MeshRendererDelegate& mr)
{
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
}
