#pragma once

#include "spt/feature.hpp"
#include "../renderer/mesh_renderer.hpp"
#include "../imgui/imgui_interface.hpp"

#include <random>
#include <shared_mutex>

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

struct HtPortal
{
	Vector pos;
	float pitch, yaw;
};

struct HtPortalPair
{
	HtPortal blue, orange;

	Vector CalcVagPt(bool blueEntry) const;
};

using candidate_idx = uint32_t;

struct HtCandidate
{
	HtPortalPair pair;
	float metric;
	uint32_t blueEntry : 1;
	uint32_t hasParent : 1;
	uint32_t generation : 20;
	candidate_idx parentIndex;
};

struct HtCandidateCreateParams
{
	uint32_t generation;
	bool blueEntry;
};

struct HtCandidateNudgeParams
{
	uint32_t generation;
	bool blueEntry;
	bool allowBlueNudge;
	bool allowOrangeNudge;
	candidate_idx parent;
};

struct HtGenerationInfoRatios
{
	float keepExact = 10.f;
	float mutateStrong = 50.f;
	float mutateWeak = 20.f;
	float injectRandom = 20.f;
};

struct HtGenerationInfo
{
	size_t generationSize;
	// keep this many of the strongest candidates without changing them
	size_t keepExact;
	// choose this many of the strongest and mutate them
	size_t mutateStrong;
	// choose this many of the remaining weak ones and mutate them
	size_t mutateWeak;
	// add this many randomly choosen portals
	size_t injectRandom;

	HtGenerationInfo(size_t generationSize, const HtGenerationInfoRatios& ratios) : generationSize(generationSize)
	{
		float s = 1.f / (keepExact + mutateStrong + mutateWeak + injectRandom);
		keepExact = generationSize * s * ratios.keepExact;
		mutateStrong = generationSize * s * ratios.mutateStrong;
		mutateWeak = generationSize * s * ratios.mutateWeak;
		injectRandom = generationSize * s * ratios.injectRandom;
	}
};

class HtIWorld
{
public:
	virtual HtCandidate CreateRandomCandidate(const HtCandidateCreateParams& params) const = 0;
	virtual HtCandidate NudgeCandidate(const HtCandidateNudgeParams& params) const = 0;
	virtual ~HtIWorld() {};
};

class HtContinuousWorld : public HtIWorld
{
public:
	virtual HtCandidate CreateRandomCandidate(const HtCandidateCreateParams& params) const override;
	virtual HtCandidate NudgeCandidate(const HtCandidateNudgeParams& params) const override;
};

class HtWorker
{
	std::thread thread;
	std::condition_variable cv;
	std::shared_mutex mtx;

	const HtGenerationInfo genInfo;
	std::minstd_rand rng;

	std::shared_ptr<const HtIWorld> world;

	std::vector<HtCandidate> candidateHistory;
	std::vector<candidate_idx> curGeneration;

	void Stop();
	void WorkerLoop();

public:
	HtWorker(size_t generationSize, const HtGenerationInfoRatios& genRatios, std::shared_ptr<const HtIWorld> world);

	~HtWorker()
	{
		Stop();
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
	static std::shared_mutex targetMtx;
	std::unique_ptr<HtWorker> worker;

	void ImGuiTabCallbackImpl();
	bool ImGuiPointTargetConfig(HtVagPointTarget& target);
	bool ImGuiBoxTargetConfig(HtVagBoxTarget& target);

	static void ImGuiTabCallback()
	{
		spt_vag_hunter_feat.ImGuiTabCallbackImpl();
	}

	void OnRenderSignal(MeshRendererDelegate& mr);

} inline spt_vag_hunter_feat;
