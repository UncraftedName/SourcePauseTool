#include "stdafx.h"

#include "vphysics_interface.h"
#define GAME_DLL
#include "cbase.h"
#include "physics_collisionevent.h"
#include "CommandBuffer.h"
#include "SoundEmitterSystem\isoundemittersystembase.h"

#include "logger.hpp"
#include "signals.hpp"
#include "interfaces.hpp"
#include "..\ent_props.hpp"
#include "spt\spt-serverplugin.hpp"

class LoggerHooks : public FeatureWrapper<LoggerHooks>
{
public:
	std::vector<std::tuple<std::string, size_t, std::string, void**, void*>> queuedHooks;

	CGlobalVars** gpGlobals;
	uint* IVP_RAND_SEED;

	struct
	{
		// base entity
		int m_vecAbsOrigin;
		// grab controller
		int m_attachedEntity;
	} offsets;

protected:
	void InitHooks() override;
	void LoadFeature() override;
	void UnloadFeature() override;

private:
	// don't include tick signal - that's handled from logger.cpp
	void OnFrameSignal();
	void OnRestoreSignal(void*);
	void OnPauseSignal(void*, bool paused);
};

static LoggerHooks spt_lh;

#define ENT_PROP(ptr, type, name) *reinterpret_cast<type*>(reinterpret_cast<uint32_t>(ptr) + spt_lh.offsets.name)

#define ENT_POS(ptr) (ptr ? ENT_PROP(ptr, Vector, m_vecAbsOrigin) : Vector{VEC_T_NAN})

void LoggerHooks::InitHooks()
{
	for (const auto& [moduleName, off, funcName, origPtr, hookPtr] : queuedHooks)
		AddOffsetHook(moduleName, off, funcName.c_str(), origPtr, hookPtr);

	AddOffsetHook("server", 0x6de068, "gpGlobals", (void**)&gpGlobals);
	AddOffsetHook("vphysics", 0xd2400, "IVP_RAND_SEED", (void**)&IVP_RAND_SEED);
}

void LoggerHooks::LoadFeature()
{
	FrameSignal.Connect(this, &LoggerHooks::OnFrameSignal);
	FinishRestoreSignal.Connect(this, &LoggerHooks::OnRestoreSignal);
	SetPausedSignal.Connect(this, &LoggerHooks::OnPauseSignal);

	offsets.m_vecAbsOrigin = spt_entprops.GetFieldOffset("CBaseEntity", "m_vecAbsOrigin", true);
	offsets.m_attachedEntity = spt_entprops.GetFieldOffset("CGrabController", "m_attachedEntity", true);
}

void LoggerHooks::UnloadFeature()
{
	queuedHooks.clear();
}

void LoggerHooks::OnFrameSignal()
{
	URINATE_SIMPLE(false);
}

void LoggerHooks::OnRestoreSignal(void*)
{
	URINATE_CHECK_LOAD_BEGIN();
}

void LoggerHooks::OnPauseSignal(void*, bool paused)
{
	URINATE_WITH_INFO(false, { uu.Spew("paused: %d", paused); });
}

/****************************** BASIC OFFSET HOOKS BELOW ******************************/

struct CPhysicsObject : public IPhysicsObject
{
};

struct CPhysicsEnvironment : public IPhysicsEnvironment
{
};

struct CShadowController
{
	char _pad0[0xb0];
	CPhysicsObject* m_pObject;
};

struct CGrabController
{
};

struct IVP_Environment
{
};

struct IVP_Real_Object
{
};

struct IVP_U_Matrix
{
};

struct CWeaponPhysCannon
{
};

struct CPlayerPickupController
{
};

class CUserCmd;

// replicate from vphysics_sound.h
struct soundlist_t
{
	struct impactsound_t
	{
	};

	CUtlVector<impactsound_t> elements;
};

#define BASIC_OFFSET_HOOK(moduleName, offset, retType, callType, funcName, params, hookBody) \
	static retType(callType* ORIG_##funcName) params; \
	static retType callType funcName params \
	{ \
		hookBody \
	} \
	namespace CONCATENATE(_hook, __COUNTER__) \
	{ \
		struct _QueueHook \
		{ \
			_QueueHook() \
			{ \
				spt_lh.queuedHooks.emplace_back(#moduleName, offset, #funcName, (void**)&ORIG_##funcName, funcName); \
			} \
		} _inst; \
	}

// a few helpful things for logging/hooking functions super high up the callstack e.g. _Host_RunFrame (where unloading normally would cause control flow to return into this unloaded plugin)

bool g_deferPluginUnload = false;
int g_hookCallDepth = 0;

struct HighCallstackFuncScope
{
	HighCallstackFuncScope()
	{
		g_hookCallDepth++;
	}

	~HighCallstackFuncScope()
	{
		g_hookCallDepth--;
		if (g_deferPluginUnload && g_hookCallDepth == 0)
		{
			extern CSourcePauseTool g_SourcePauseTool;
			g_SourcePauseTool.Unload();
		}
	}
};

BASIC_OFFSET_HOOK(engine, 0x49620, void, __cdecl, _Host_RunFrame, (float time_), {
	HighCallstackFuncScope _h{};
	URINATE_SIMPLE(true);
	ORIG__Host_RunFrame(time_);
});

BASIC_OFFSET_HOOK(engine, 0x48500, void, __cdecl, _Host_RunFrame_Server, (bool finalTick), {
	HighCallstackFuncScope _h{};
	URINATE_SIMPLE(true);
	ORIG__Host_RunFrame_Server(finalTick);
});

BASIC_OFFSET_HOOK(engine, 0x485e0, void, __cdecl, _Host_RunFrame_Client, (bool framefinished), {
	URINATE_SIMPLE(true);
	ORIG__Host_RunFrame_Client(framefinished);
});

BASIC_OFFSET_HOOK(engine, 0x46cd0, void, __cdecl, _Host_RunFrame_Input, (float accumulated_extra_samples, bool bFinalTick), {
	URINATE_SIMPLE(true);
	ORIG__Host_RunFrame_Input(accumulated_extra_samples, bFinalTick);
});

BASIC_OFFSET_HOOK(engine, 0x48740, void, __cdecl, _Host_RunFrame_Render, (), {
	URINATE_SIMPLE(true);
	ORIG__Host_RunFrame_Render();
});

BASIC_OFFSET_HOOK(engine, 0x46db0, void, __cdecl, _Host_RunFrame_Sound, (), {
	URINATE_SIMPLE(true);
	ORIG__Host_RunFrame_Sound();
});

// not logging, just getting ORIG ptr
BASIC_OFFSET_HOOK(server, 0xd6220, int, __fastcall, CBaseEntity__VPhysicsGetObjectList, (CBaseEntity * thisptr, int edx, CPhysicsObject** pList, int listMax), {
	return ORIG_CBaseEntity__VPhysicsGetObjectList(thisptr, edx, pList, listMax);
});

BASIC_OFFSET_HOOK(server,
                  0x436ce0,
                  void,
                  __fastcall,
                  CGrabController__AttachEntity,
                  (void* thisptr, int edx, CBasePlayer* pPlayer, CBaseEntity* pEntity, IPhysicsObject* pPhys, bool bIsMegaPhysCannon, const Vector& vGrabPosition, bool bUseGrabPosition),
                  {
	                  URINATE_WITH_INFO(false, {
		                  uu.Spew("ent: (%d) \"%s\", grab pos: " V_FMT ", use grab pos: %d, ent pos: " V_FMT,
		                          pEntity->GetRefEHandle().GetEntryIndex(),
		                          pEntity->GetClassname(),
		                          V_UNP(vGrabPosition),
		                          bUseGrabPosition,
		                          V_UNP(ENT_POS(pEntity)));
	                  });
	                  ORIG_CGrabController__AttachEntity(thisptr, edx, pPlayer, pEntity, pPhys, bIsMegaPhysCannon, vGrabPosition, bUseGrabPosition);
                  });

BASIC_OFFSET_HOOK(server, 0x4361f0, void, __fastcall, CGrabController__DetachEntity, (CGrabController * thisptr, int edx, bool bClearVelocity), {
	EHANDLE hAttached = ENT_PROP(thisptr, EHANDLE, m_attachedEntity);
	URINATE_WITH_INFO(true, {
		if (!hAttached.IsValid())
		{
			uu.Spew("null entity");
			return;
		}
		CBaseEntity* attached = interfaces::engine_server->PEntityOfEntIndex(hAttached.GetEntryIndex())->GetIServerEntity()->GetBaseEntity();
		CPhysicsObject* pObj = (CPhysicsObject*)attached->VPhysicsGetObject();
		if (pObj)
		{
			Vector vel;
			AngularImpulse impulse;
			pObj->GetVelocity(&vel, &impulse);
			uu.Spew("clear vel: %d, flags: %d, pos: " V_FMT ", vphys vel: " V_FMT ", vphys impulse: " V_FMT,
			        bClearVelocity,
			        *(ushort*)((uint32_t)pObj + 0x2c),
			        V_UNP(ENT_POS(attached)),
			        V_UNP(vel),
			        V_UNP(impulse));
		}
		else
		{
			uu.Spew("clear vel: %d, no vphys objects", bClearVelocity);
		}
	});
	ORIG_CGrabController__DetachEntity(thisptr, edx, bClearVelocity);
});

BASIC_OFFSET_HOOK(server, 0x0193b30, penetrateevent_t&, __fastcall, CCollisionEvent__FindOrAddPenetrateEvent, (CCollisionEvent * thisptr, int edx, CBaseEntity* pEntity0, CBaseEntity* pEntity1), {
	penetrateevent_t& pEvent = ORIG_CCollisionEvent__FindOrAddPenetrateEvent(thisptr, edx, pEntity0, pEntity1);
	// swap so that we don't get pointer dependent ordering in the logs
	if (pEntity0->GetRefEHandle().GetEntryIndex() > pEntity1->GetRefEHandle().GetEntryIndex())
		std::swap(pEntity0, pEntity1);
	URINATE_WITH_INFO(false, {
		uu.Spew("(%d) \"%s\" / (%d) \"%s\", event time: %f",
		        pEntity0->GetRefEHandle().GetEntryIndex(),
		        pEntity0->GetClassname(),
		        pEntity1->GetRefEHandle().GetEntryIndex(),
		        pEntity1->GetClassname(),
		        (**spt_lh.gpGlobals).curtime - pEvent.startTime);
	});
	return pEvent;
});

BASIC_OFFSET_HOOK(server, 0x190430, void, __cdecl, PhysFrame, (float deltaTime), {
	URINATE_SIMPLE(true);
	ORIG_PhysFrame(deltaTime);
});

BASIC_OFFSET_HOOK(server, 0x1528c0, void, __fastcall, CServerGameDLL__GameFrame, (IServerGameDLL * thisptr, int edx, bool simulating), {
	URINATE_SIMPLE(true);
	ORIG_CServerGameDLL__GameFrame(thisptr, edx, simulating);
});

BASIC_OFFSET_HOOK(server, 0x1c4e40, void, __fastcall, CBasePlayer__VPhysicsShadowUpdate, (CBasePlayer * thisptr, int edx, CPhysicsObject* pPhysics), {
	URINATE_WITH_INFO(true, { uu.Spew("player pos: " V_FMT, V_UNP(ENT_POS(thisptr))); });
	ORIG_CBasePlayer__VPhysicsShadowUpdate(thisptr, edx, pPhysics);
});

BASIC_OFFSET_HOOK(vphysics, 0x17b90, void, __fastcall, CPhysicsObject__AddVelocity, (CPhysicsObject * thisptr, int edx, Vector* vec, AngularImpulse* vecAng), {
	Vector inVecCopy = *vec;
	URINATE_WITH_INFO(true, {
		Vector vphysVel;
		AngularImpulse vphysImpulse;
		thisptr->GetVelocity(&vphysVel, &vphysImpulse);
		uu.Spew("phys vel: " V_FMT ", vphys impulse: " V_FMT ", add vel: " V_FMT, V_UNP(vphysVel), V_UNP(vphysImpulse), V_UNP(inVecCopy));
	});
	ORIG_CPhysicsObject__AddVelocity(thisptr, edx, vec, vecAng);
});

BASIC_OFFSET_HOOK(vphysics, 0x129b0, void, __fastcall, CPhysicsEnvironment__Simulate, (CPhysicsEnvironment * thisptr, int edx, float deltatime), {
	URINATE_SIMPLE(true);
	ORIG_CPhysicsEnvironment__Simulate(thisptr, edx, deltatime);
});

// too much info for now, reenable later
BASIC_OFFSET_HOOK(server, 0xea710, void, __fastcall, CBaseEntity__VPhysicsUpdate, (CBaseEntity * thisptr, int edx, CPhysicsObject* pPhysics), {
	URINATE_WITH_INFO(true, {
		Vector vphysPos;
		QAngle vphysAng;
		pPhysics->GetPosition(&vphysPos, nullptr);
		uu.Spew("(%d) \"%s\", pos: " V_FMT ", vphys pos: " V_FMT ", vphys ang: " V_FMT,
		        thisptr->GetRefEHandle().GetEntryIndex(),
		        thisptr->GetClassname(),
		        V_UNP(ENT_POS(thisptr)),
		        V_UNP(vphysPos),
		        V_UNP(vphysAng));
	});
	ORIG_CBaseEntity__VPhysicsUpdate(thisptr, edx, pPhysics);
});

// the object I want doesn't have a shadow controller
/*BASIC_OFFSET_HOOK(vphysics, 0x1a410, void, __fastcall, CShadowController__do_simulation_controller, (CShadowController * thisptr, int edx, void* es, void* cores), {
	URINATE_WITH_INFO(true, {
		CBaseEntity* ent = (CBaseEntity*)thisptr->m_pObject->GetGameData();
		Vector vphysPos;
		thisptr->m_pObject->GetPosition(&vphysPos, nullptr);
		uu.Spew("(%d) \"%s\", vphys pos: " V_FMT, ent->GetRefEHandle().GetEntryIndex(), ent->GetClassname(), V_UNP(vphysPos));
	});
	ORIG_CShadowController__do_simulation_controller(thisptr, edx, es, cores);
});*/

BASIC_OFFSET_HOOK(server,
                  0x435a20,
                  IMotionEvent::simresult_e,
                  __fastcall,
                  CGrabController__Simulate,
                  (CGrabController * thisptr, int edx, IPhysicsMotionController* pController, CPhysicsObject* pObject, float deltaTime, Vector& linear, AngularImpulse& angular),
                  {
	                  URINATE_WITH_INFO(true, {
		                  CBaseEntity* ent = (CBaseEntity*)pObject->GetGameData();
		                  Vector vphysVel;
		                  AngularImpulse vphysImpulse;
		                  pObject->GetVelocity(&vphysVel, &vphysImpulse);
		                  uu.Spew("(%d) \"%s\", vphys vel: " V_FMT ", vphys impulse: " V_FMT, ent->GetRefEHandle().GetEntryIndex(), ent->GetClassname(), V_UNP(vphysVel), V_UNP(vphysImpulse));
	                  });
	                  return ORIG_CGrabController__Simulate(thisptr, edx, pController, pObject, deltaTime, linear, angular);
                  });

BASIC_OFFSET_HOOK(vphysics, 0x3e370, void, __fastcall, IVP_Environment__simulate_time_step, (IVP_Environment * thisptr, int edx, float sub_psi_time), {
	URINATE_WITH_INFO(true, { uu.Spew("ivp seed: %u", *spt_lh.IVP_RAND_SEED); });
	ORIG_IVP_Environment__simulate_time_step(thisptr, edx, sub_psi_time);
});

// not triggered?
BASIC_OFFSET_HOOK(vphysics, 0x331c0, void, __fastcall, IVP_Real_Object__set_new_m_object_f_core, (IVP_Real_Object * thisptr, int edx, IVP_U_Matrix* new_m_object_f_core), {
	URINATE_SIMPLE(true);
	ORIG_IVP_Real_Object__set_new_m_object_f_core(thisptr, edx, new_m_object_f_core);
});

// not triggered
BASIC_OFFSET_HOOK(server, 0x43bba0, void, __fastcall, CWeaponPhysCannon__DetachObject, (CWeaponPhysCannon * thisptr, int edx, bool playSound, bool wasLaunched), {
	URINATE_SIMPLE(true);
	ORIG_CWeaponPhysCannon__DetachObject(thisptr, edx, playSound, wasLaunched);
});

BASIC_OFFSET_HOOK(server, 0x4389a0, void, __fastcall, CPlayerPickupController__Shutdown, (CPlayerPickupController * thisptr, int edx, bool bThrown), {
	URINATE_SIMPLE(true);
	ORIG_CPlayerPickupController__Shutdown(thisptr, edx, bThrown);
});

BASIC_OFFSET_HOOK(server,
                  0x439ff0,
                  void,
                  __fastcall,
                  CPlayerPickupController__Use,
                  (CPlayerPickupController * thisptr, int edx, CBaseEntity* pActivator, CBaseEntity* pCaller, USE_TYPE useType, float value),
                  {
	                  URINATE_SIMPLE(true);
	                  ORIG_CPlayerPickupController__Use(thisptr, edx, pActivator, pCaller, useType, value);
                  });

BASIC_OFFSET_HOOK(server, 0x23380, AngularImpulse, __cdecl, RandomAngularImpulse_MINE, (float minVal, float maxVal), {
	AngularImpulse ret{};
	{
		URINATE_WITH_INFO(true, {
			if (isPre)
				uu.Spew("min: %f, max: %f", minVal, maxVal);
			else
				uu.Spew("min: %f, max: %f, impulse: " V_FMT, minVal, maxVal, V_UNP(ret));
		});
		ret = ORIG_RandomAngularImpulse_MINE(minVal, maxVal);
	}
	return ret;
});

BASIC_OFFSET_HOOK(vstdlib, 0x7ae0, int, __fastcall, CUniformRandomStream__RandomInt, (CUniformRandomStream * thisptr, int edx, int iLow, int iHigh), {
	int ret = -666;
	if (iLow == iHigh)
	{
		ret = ORIG_CUniformRandomStream__RandomInt(thisptr, edx, iLow, iHigh);
		URINATE_WITH_INFO(false, { uu.Spew("low: %d, high: %d, value: %d", iLow, iHigh, ret); });
	}
	else
	{
		URINATE_WITH_INFO(true, {
			if (isPre)
				uu.Spew("low: %d, high: %d", iLow, iHigh);
			else
				uu.Spew("low: %d, high: %d, value: %d", iLow, iHigh, ret);
		});
		ret = ORIG_CUniformRandomStream__RandomInt(thisptr, edx, iLow, iHigh);
	}
	return ret;
});

BASIC_OFFSET_HOOK(vstdlib, 0x7a30, float, __fastcall, CUniformRandomStream__RandomFloat, (CUniformRandomStream * thisptr, int edx, float flLow, float flHigh), {
	float ret = 0;
	{
		URINATE_WITH_INFO(true, {
			if (isPre)
				uu.Spew("low: %f, high: %f", flLow, flHigh);
			else
				uu.Spew("low: %f, high: %f, value: %f", flLow, flHigh, ret);
		});
		ret = ORIG_CUniformRandomStream__RandomFloat(thisptr, edx, flLow, flHigh);
	}
	return ret;
});

BASIC_OFFSET_HOOK(vstdlib, 0x77d0, int, __fastcall, CUniformRandomStream__GenerateRandomNumber, (CUniformRandomStream * thisptr, int edx), {
	/*int ret = 0;
	{
		URINATE_WITH_INFO(true, {
			if (isPre)
				uu.Spew("");
			else
				uu.Spew("value: %d", ret);
		});
		ret = ORIG_CUniformRandomStream__GenerateRandomNumber(thisptr, edx);
	}
	return ret;*/
	int ret = ORIG_CUniformRandomStream__GenerateRandomNumber(thisptr, edx);
	URINATE_WITH_INFO(false, { uu.Spew("value: %d", ret); });
	return ret;
});

BASIC_OFFSET_HOOK(vstdlib, 0x7760, void, __fastcall, CUniformRandomStream__SetSeed, (CUniformRandomStream * thisptr, int edx, int iSeed), {
	URINATE_WITH_INFO(false, { uu.Spew("%d", iSeed); });
	ORIG_CUniformRandomStream__SetSeed(thisptr, edx, iSeed);
});

BASIC_OFFSET_HOOK(vstdlib, 0x76d0, void, __cdecl, RandomSeed_MINE, (int iSeed), {
	URINATE_SIMPLE(true);
	ORIG_RandomSeed_MINE(iSeed);
});

BASIC_OFFSET_HOOK(server, 0x234f50, int, __cdecl, SharedRandomInt_MINE, (const char* sharedname, int iMinVal, int iMaxVal, int additionalSeed), {
	int ret = 0;
	{
		URINATE_WITH_INFO(true, {
			if (isPre)
				uu.Spew("shared name: \"%s\", min: %d, max: %d, additional: %d", sharedname, iMinVal, iMaxVal, additionalSeed);
			else
				uu.Spew("shared name: \"%s\", min: %d, max: %d, additional: %d, value: %d", sharedname, iMinVal, iMaxVal, additionalSeed, ret);
		});
		ret = ORIG_SharedRandomInt_MINE(sharedname, iMinVal, iMaxVal, additionalSeed);
	}
	return ret;
});

BASIC_OFFSET_HOOK(SoundEmitterSystem,
                  0x1e40,
                  bool,
                  __fastcall,
                  CSoundEmitterSystemBase__GetParametersForSound,
                  (ISoundEmitterSystemBase * thisptr, int edx, const char* soundname, CSoundParameters& params, gender_t gender, bool isbeingemitted),
                  {
	                  URINATE_WITH_INFO(true, { uu.Spew("sound: \"%s\", gender: %d", soundname, gender); });
	                  return ORIG_CSoundEmitterSystemBase__GetParametersForSound(thisptr, edx, soundname, params, gender, isbeingemitted);
                  });

BASIC_OFFSET_HOOK(server, 0xf6100, void, __fastcall, CBasePlayer__PlayerStepSound, (CBasePlayer * thisptr, int edx, Vector& vecOrigin, surfacedata_t* psurface, float fvol, bool force), {
	URINATE_SIMPLE(true);
	ORIG_CBasePlayer__PlayerStepSound(thisptr, edx, vecOrigin, psurface, fvol, force);
});

BASIC_OFFSET_HOOK(server, 0x419c60, bool, __fastcall, CPortal_Player__UseFoundEntity, (CBasePlayer * thisptr, int edx, CBaseEntity* pUseEntity), {
	URINATE_SIMPLE(true);
	return ORIG_CPortal_Player__UseFoundEntity(thisptr, edx, pUseEntity);
});

BASIC_OFFSET_HOOK(server, 0x193400, void, __cdecl, PlayImpactSounds_MINE, (soundlist_t & list), {
	URINATE_WITH_INFO(true, { uu.Spew("num sounds: %d", list.elements.Count()); });
	ORIG_PlayImpactSounds_MINE(list);
});

BASIC_OFFSET_HOOK(vstdlib, 0x7ba0, float, __fastcall, CGaussianRandomStream__RandomFloat, (CGaussianRandomStream * thisptr, int edx, float flMean, float flStdDev), {
	float ret = 0;
	{
		URINATE_WITH_INFO(true, {
			if (isPre)
				uu.Spew("mean: %f, stddev: %f", flMean, flStdDev);
			else
				uu.Spew("mean: %f, stddev: %f, value: %f", flMean, flStdDev, ret);
		});
		ret = ORIG_CGaussianRandomStream__RandomFloat(thisptr, edx, flMean, flStdDev);
	}
	return ret;
});

BASIC_OFFSET_HOOK(server, 0x2dc9f0, void, __fastcall, CHL2_Player__UpdateWeaponPosture, (CBasePlayer * thisptr, int edx), {
	URINATE_SIMPLE(true);
	ORIG_CHL2_Player__UpdateWeaponPosture(thisptr, edx);
});

BASIC_OFFSET_HOOK(server, 0x1bd780, void, __fastcall, CBasePlayer__PlayerRunCommand, (CBasePlayer * thisptr, int edx, CUserCmd* ucmd, void* moveHelper), {
	URINATE_SIMPLE(true);
	ORIG_CBasePlayer__PlayerRunCommand(thisptr, edx, ucmd, moveHelper);
});

BASIC_OFFSET_HOOK(server, 0x39290, void, __fastcall, CAI_BaseNPC__NPCThink, (CBaseEntity * thisptr, int edx), {
	URINATE_WITH_INFO(true, { uu.Spew("(%d) \"%s\"", thisptr->GetRefEHandle().GetEntryIndex(), thisptr->GetClassname()); });
	ORIG_CAI_BaseNPC__NPCThink(thisptr, edx);
});

BASIC_OFFSET_HOOK(SoundEmitterSystem,
                  0x4790,
                  int,
                  __fastcall,
                  CSoundEmitterSystemBase__FindBestSoundForGender,
                  (ISoundEmitterSystemBase* thisptr, int edx, SoundFile* pSoundnames, int c, gender_t gender),
                  {
	                  URINATE_SIMPLE(true);
	                  return ORIG_CSoundEmitterSystemBase__FindBestSoundForGender(thisptr, edx, pSoundnames, c, gender);
                  });
