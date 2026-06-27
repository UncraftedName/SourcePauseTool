#include "stdafx.hpp"
#include "hunt.hpp"

Vector HtPortalPair::CalcVagPt(HtPortalColor entryColor) const
{
	const HtPortal& entry = p[entryColor];
	const HtPortal& exit = p[1 - entryColor];
	Vector vagPt = entry.pos;
	matrix3x4_t mat;
	AngleIMatrix(QAngle{entry.pitch, entry.yaw, 0.f}, entry.pos, mat);
	utils::VectorTransform(mat, vagPt);
	vagPt[0] = -vagPt[0];
	vagPt[1] = -vagPt[1];
	AngleMatrix(QAngle{exit.pitch, exit.yaw, 0.f}, exit.pos, mat);
	utils::VectorTransform(mat, vagPt);
	return vagPt;
}

void HtCandidate::RecalcDistMetric()
{
	// TODO the target being a global is a bit weird and cringe... maybe at least make a class {target, targetMtx} wrapper?
	Vector vagPt = pp.CalcVagPt((HtPortalColor)entryColor);
	std::shared_lock lk(spt_vag_hunter_feat.targetMtx);
	metric = spt_vag_hunter_feat.vagTarget.DistTo(vagPt);
}

HtCandidate HtContinuousWorld::CreateRandomCandidate(const HtCandidateCreateParams& params, ht_rng& rng) const
{
	std::uniform_int_distribution portalTypeDist(0, 5);
	std::uniform_real_distribution yawDist(-180.f, 180.f);
	std::uniform_real_distribution posDist(-5000.f, 5000.f);

	HtCandidate ret;

	for (int i = 0; i < 2; i++)
	{
		auto& p = ret.pp.p[i];
		p.pos = Vector(posDist(rng), posDist(rng), posDist(rng));
		int portalType = portalTypeDist(rng);
		switch (portalType)
		{
		case 0: // floor
			p.pitch = 90.f;
			p.yaw = yawDist(rng);
			break;
		case 1: // ceiling
			p.pitch = -90.f;
			p.yaw = yawDist(rng);
			break;
		default: // wall
			p.pitch = 0.f;
			p.yaw = (portalType - 3) * 90.f;
			break;
		}
	}

	ret.entryColor = params.entryColor;
	ret.generation = params.generation;
	ret.myIdx = params.newCandIdx;
	ret.parentIndex = HT_INVALID_CANDIDATE_IDX;
	ret.RecalcDistMetric();

	return ret;
}

HtCandidate HtContinuousWorld::NudgeCandidate(const HtCandidateNudgeParams& params, ht_rng& rng) const
{
	// select portal to nudge
	const HtPortalPair& sourcePp = params.sourceCand->pp;
	Assert(params.allowBlueNudge || params.allowOrangeNudge);
	std::uniform_int_distribution boolDist(0, 1);
	bool nudgeOrange = params.allowBlueNudge && params.allowOrangeNudge ? boolDist(rng) : params.allowOrangeNudge;

	HtCandidate ret;
	std::memcpy(&ret.pp, &sourcePp, sizeof HtPortalPair);
	HtPortal& nudgePortal = ret.pp.p[nudgeOrange];

	std::uniform_real_distribution posDist(-500.f, 500.f);
	nudgePortal.pos += Vector(posDist(rng), posDist(rng), posDist(rng));

	if (std::fabsf(std::fabsf(nudgePortal.pitch) - 90.f) < 0.0001f)
	{
		// floor/ceiling
		std::uniform_real_distribution yawDist(0.f, 360.f);
		nudgePortal.yaw += yawDist(rng);
		nudgePortal.yaw = std::fmodf(nudgePortal.yaw + 180.f, 360.f) - 180.f;
	}

	ret.entryColor = params.sourceCand->entryColor;
	ret.generation = params.generation;
	ret.myIdx = params.newCandIdx;
	ret.parentIndex = params.sourceCand->myIdx;
	ret.RecalcDistMetric();

	return ret;
}

HtWorker::HtWorker(size_t generationSize,
                   const HtGenerationInfoRatios& genRatios,
                   std::shared_ptr<const HtIWorld> world)
    : genInfo(generationSize, genRatios), world(std::move(world))
{
	Assert(generationSize > 0);

	thread = std::thread(&HtWorker::WorkerLoop, this);
}

void HtWorker::MakeNewGeneration()
{
	std::unique_lock lk(mtx);
	cv.wait(lk, [this]() { return state == WORK_IDLE || state == WORK_STOP; });
	Assert(state != WORK_STOP);
	state = WORK_MAKE_GENERATION;
	cv.notify_one();
}

void HtWorker::Stop()
{
	if (!thread.joinable())
		return;
	mtx.lock();
	this->state = WORK_STOP;
	mtx.unlock();
	cv.notify_one();
	thread.join();
}

void HtWorker::WorkerLoop()
{
	HtGenerationInfoRatios firstGenRatios;
	HtGenerationInfo firstGenInfo(genInfo.generationSize, firstGenRatios);

	for (;;)
	{
		{
			std::unique_lock lk(mtx);
			if (state != WORK_STOP)
				state = WORK_IDLE;
			cv.notify_one();
			cv.wait(lk, [this]() { return state == WORK_MAKE_GENERATION || state == WORK_STOP; });
			if (state == WORK_STOP)
				return;
		}
		size_t generation = nGenerations.load(std::memory_order_acquire);
		WorkerMakeGeneration(generation == 0 ? firstGenInfo : genInfo, generation);
		nGenerations.fetch_add(1, std::memory_order_release);
	}
}

void HtWorker::WorkerMakeGeneration(const HtGenerationInfo& curGenInfo, size_t generation)
{
	// lock mutex to edit candidate history
	std::unique_lock lk(mtx);

	newGenerationScratch.clear();

	// copy the best candidates
	std::copy(lastGeneration.begin(),
	          lastGeneration.begin() + curGenInfo.keepExact,
	          std::back_inserter(newGenerationScratch));

	std::uniform_real_distribution realDist(0.0f, 1.0f);
	std::uniform_int_distribution<size_t> weakIdxDist(curGenInfo.keepExact, curGenInfo.generationSize - 1);
	float invNCandidates = 1.f / curGenInfo.generationSize;

	HtCandidateNudgeParams nudgeParams{
	    .generation = generation,
	    // TODO
	    .entryColor = HT_ENTRY_BLUE,
	    .allowBlueNudge = true,
	    .allowOrangeNudge = true,
	};

	for (size_t doStrong = 0; doStrong < 2; doStrong++)
	{
		size_t nMutations = doStrong ? curGenInfo.mutateStrong : curGenInfo.mutateWeak;
		for (size_t i = 0; i < nMutations; i++)
		{
			size_t genSourceIdx;
			if (doStrong)
			{
				// linear distribution to bias strong candidates
				genSourceIdx = (size_t)(std::powf(realDist(rng), .5f) * invNCandidates);
			}
			else
			{
				genSourceIdx = weakIdxDist(rng);
			}

			nudgeParams.sourceCand = &candidateHistory[lastGeneration[genSourceIdx]];
			nudgeParams.newCandIdx = candidateHistory.size();
			candidateHistory.push_back(world->NudgeCandidate(nudgeParams, rng));
			newGenerationScratch.push_back(nudgeParams.newCandIdx);
		}
	}

	HtCandidateCreateParams createParams{
	    .generation = generation,
	    // TODO
	    .entryColor = HT_ENTRY_BLUE,
	};

	// create some new random candidates
	for (size_t i = 0; i < curGenInfo.injectRandom; i++)
	{
		createParams.newCandIdx = candidateHistory.size();
		candidateHistory.push_back(world->CreateRandomCandidate(createParams, rng));
		newGenerationScratch.push_back(createParams.newCandIdx);
	}

	std::swap(lastGeneration, newGenerationScratch);

	// sort by distance metric
	std::ranges::sort(lastGeneration, std::less{}, [this](auto& idx) { return candidateHistory[idx].metric; });
}
