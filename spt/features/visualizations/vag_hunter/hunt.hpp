#pragma once

#include "spt/feature.hpp"
#include "../renderer/mesh_renderer.hpp"
#include "../imgui/imgui_interface.hpp"

class HtIVagTarget
{
public:
	virtual float DistTo(const Vector& pt) const = 0;
	virtual ~HtIVagTarget() {};
};

// a point target is one or more points, distance is the minimal distance to any of the points
class HtVagPointTarget : public HtIVagTarget
{
public:
	std::vector<Vector> points;

	bool Empty() const
	{
		return points.empty();
	}

	void Clear()
	{
		points.clear();
	}

	virtual float DistTo(const Vector& pt) const override
	{
		auto it = std::ranges::min_element(points, std::less{}, [&](const Vector& p) { return p.DistTo(pt); });
		return it == points.end() ? INFINITY : it->DistTo(pt);
	}
};

struct HtAabb
{
	Vector mins, maxs;
};

/*
* A box target is one or more boxes or one or more points inside of the boxes. If the compare
* point is inside the AABBs, the point targets are used. If the compare point is outside, the
* distance to the AABB + internal distance.
*/
class HtVagBoxTarget : public HtIVagTarget
{
protected:
	// max distance from any interior point to AABB face
	float maxInternalDist = 0.f;

public:
	HtVagPointTarget pointTarget;
	std::vector<HtAabb> aabbs;

	// user submitted one corner, now we get the other
	std::optional<Vector> pendingAabbTargetCorner;

	bool Empty() const
	{
		return pointTarget.Empty() && aabbs.empty();
	}

	void Clear()
	{
		pointTarget.Clear();
		aabbs.clear();
		maxInternalDist = 0.f;
	}

	// this should be called any time the point or AABB list is changed
	virtual void RecalcMaxInternalDist()
	{
		maxInternalDist = 0.f;
		if (pointTarget.Empty() || aabbs.empty())
			return;

		for (auto& interiorPt : pointTarget.points)
		{
			for (auto& aabb : aabbs)
			{
				for (int i = 0; i < 8; i++)
				{
					Vector aabbCorner{
					    (i & 1) ? aabb.mins[0] : aabb.maxs[0],
					    (i & 2) ? aabb.mins[1] : aabb.maxs[1],
					    (i & 4) ? aabb.mins[2] : aabb.maxs[2],
					};
					maxInternalDist = std::max(maxInternalDist, interiorPt.DistToSqr(aabbCorner));
				}
			}
		}
		maxInternalDist = std::sqrtf(maxInternalDist);
	}

	virtual float DistTo(const Vector& pt) const override
	{
		if (aabbs.empty())
			return pointTarget.DistTo(pt);

		float minDist = INFINITY;
		for (auto& aabb : aabbs)
		{
			minDist = std::min(minDist, CalcSqrDistanceToAABB(aabb.mins, aabb.maxs, pt));
			if (minDist <= 0.f)
				break;
		}
		if (minDist <= 0.f)
			return pointTarget.Empty() ? 0.f : pointTarget.DistTo(pt);
		return std::sqrtf(minDist) + maxInternalDist;
	}
};

class VagHunterHuntFeature : public FeatureWrapper<VagHunterHuntFeature>
{
protected:
	virtual void LoadFeature() override
	{
		if (!spt_meshRenderer.signal.Works || !SptImGui::Loaded())
			return;

		spt_meshRenderer.signal.Connect(this, &VagHunterHuntFeature::OnRenderSignal);
		SptImGuiGroup::Draw_VagHunt.RegisterUserCallback(ImGuiTabCallback);
	}

private:
	HtVagBoxTarget vagTarget;

	void ImGuiTabCallbackImpl();

	static void ImGuiTabCallback()
	{
		spt_vag_hunter_feat.ImGuiTabCallbackImpl();
	}

	void OnRenderSignal(MeshRendererDelegate& mr);

} inline spt_vag_hunter_feat;
