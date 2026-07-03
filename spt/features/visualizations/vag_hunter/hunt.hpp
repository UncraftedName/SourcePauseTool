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

enum HtPortalColor : unsigned char
{
	HT_ENTRY_BLUE,
	HT_ENTRY_ORANGE,
};

struct HtPortalPair
{
	std::array<HtPortal, 2> p;

	Vector CalcVagPt(HtPortalColor entryColor) const;
};

using candidate_idx = uint32_t;
constexpr candidate_idx HT_INVALID_CANDIDATE_IDX = std::numeric_limits<candidate_idx>::max();

struct HtCandidate
{
	HtPortalPair pp;
	float metric;
	uint32_t entryColor : 1;
	uint32_t generation : 20;
	candidate_idx myIdx;
	candidate_idx parentIndex;

	void RecalcDistMetric();
};

struct HtCandidateCreateParams
{
	uint32_t generation;
	HtPortalColor entryColor;
	candidate_idx newCandIdx;
};

struct HtCandidateNudgeParams
{
	uint32_t generation;
	HtPortalColor entryColor;
	bool allowBlueNudge;
	bool allowOrangeNudge;
	HtCandidate* sourceCand;
	candidate_idx newCandIdx;
};

enum HtSampleType
{
	// keep this many of the strongest candidates without changing them
	HT_ST_KEEP_EXACT,
	// choose this many of the strongest and mutate them
	HT_ST_MUTATE_STRONG,
	// choose this many of the remaining weak ones and mutate them
	HT_ST_MUTATE_WEAK,
	// add this many randomly generated samples
	HT_ST_INJECT_RANDOM,
	// grab this many from the history
	HT_ST_MUTATE_EXPIRED,

	HT_ST_COUNT,
};

using ht_sample_ratios = std::array<float, HT_ST_COUNT>;

inline ht_sample_ratios HtCreateReasonableRatios()
{
	ht_sample_ratios ret{};
	ret[HT_ST_KEEP_EXACT] = 5.f;
	ret[HT_ST_MUTATE_STRONG] = 10.f;
	ret[HT_ST_MUTATE_WEAK] = 20.f;
	ret[HT_ST_INJECT_RANDOM] = 5.f;
	ret[HT_ST_MUTATE_EXPIRED] = 60.f;
	return ret;
}

struct HtGenerationInfo
{
	size_t generationSize;
	std::array<size_t, HT_ST_COUNT> counts;

	HtGenerationInfo(size_t generationSize, const ht_sample_ratios& ratios);
};

using ht_rng = std::minstd_rand;

class HtIWorld
{
public:
	virtual HtCandidate CreateRandomCandidate(const HtCandidateCreateParams& params, ht_rng& rng) const = 0;
	virtual HtCandidate NudgeCandidate(const HtCandidateNudgeParams& params, ht_rng& rng) const = 0;
	virtual ~HtIWorld() {};
};

class HtContinuousWorld : public HtIWorld
{
public:
	virtual HtCandidate CreateRandomCandidate(const HtCandidateCreateParams& params, ht_rng& rng) const override;
	virtual HtCandidate NudgeCandidate(const HtCandidateNudgeParams& params, ht_rng& rng) const override;
};

class HtWorker
{
	std::thread thread;
	std::condition_variable cv;

	enum WorkerState
	{
		WORK_INIT,
		WORK_IDLE,
		WORK_MAKE_GENERATION,
		WORK_STOP,
	};

	WorkerState state = WORK_INIT;

	const HtGenerationInfo genInfo;
	ht_rng rng;

	std::shared_ptr<const HtIWorld> world;

	void Stop();
	void WorkerLoop();
	void WorkerMakeGeneration(const HtGenerationInfo& curGenInfo, size_t generation);

public:
	std::atomic<size_t> nGenerations = 0;
	std::mutex mtx;
	std::vector<HtCandidate> candidateHistory;
	std::vector<candidate_idx> lastGeneration, newGenerationScratch;

	HtWorker(size_t generationSize, const ht_sample_ratios& genRatios, std::shared_ptr<const HtIWorld> world);

	void MakeNewGeneration();

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

	virtual void UnloadFeature() override
	{
		worker.reset();
	}

public:
	HtVagBoxTarget vagTarget;
	inline static std::shared_mutex targetMtx;

private:
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
