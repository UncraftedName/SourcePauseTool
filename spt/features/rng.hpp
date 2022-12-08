#pragma once

#include <unordered_set>
#include "..\feature.hpp"

#include "utlsymbol.h"
#include "SoundEmitterSystem\isoundemittersystembase.h"

// RNG prediction
class RNGStuff : public FeatureWrapper<RNGStuff>
{
public:
	int GetPredictionRandomSeed(int commandOffset);
	int commandNumber = 0;

	DECL_MEMBER_CDECL(void, ivp_srand, uint32_t seed);

protected:
	virtual bool ShouldLoadFeature() override;
	virtual void InitHooks() override;
	virtual void PreHook() override;
	virtual void LoadFeature() override;
	virtual void UnloadFeature() override;

private:
	DECL_HOOK_CDECL(void, SetPredictionRandomSeed, void* usercmd);
#ifdef OE
	DECL_HOOK_THISCALL(void, CBasePlayer__InitVCollision);
#else
	DECL_HOOK_THISCALL(void, CBasePlayer__InitVCollision, const Vector& vecAbsOrigin, const Vector& vecAbsVelocity);
#endif
	DECL_HOOK_THISCALL(void,
	                   CSoundEmitterSystemBase__EnsureAvailableSlotsForGender,
	                   SoundFile* pSoundnames,
	                   int c,
	                   gender_t gender);
	DECL_MEMBER_CDECL(void, PhysFrame, float deltaTime);

	uint32_t* IVP_RAND_SEED = nullptr;
	float* g_PhysicsHook__m_impactSoundTime = nullptr;

	std::unordered_set<UtlSymId_t> resetSounds;
};

extern RNGStuff spt_rng;
