#include "stdafx.h"

#include "vphysics_interface.h"
#define GAME_DLL
#include "cbase.h"
#include "physics_collisionevent.h"

#include "logger.hpp"
#include "signals.hpp"
#include "interfaces.hpp"
#include "..\ent_props.hpp"

struct CPhysicsObject : public IPhysicsObject
{
};

struct CPhysicsEnvironment : public IPhysicsEnvironment
{
};

class LoggerHooks : public FeatureWrapper<LoggerHooks>
{
public:
	std::vector<std::tuple<std::string, size_t, std::string, void**, void*>> queuedHooks;

	CGlobalVars** gpGlobals;

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

BASIC_OFFSET_HOOK(server, 0x4361f0, void, __fastcall, CGrabController__DetachEntity, (void* thisptr, int edx, bool bClearVelocity), {
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
			pObj->GetVelocity(&vel, nullptr);
			uu.Spew("clear vel: %d, vphys vel: " V_FMT, bClearVelocity, V_UNP(vel));
		}
		else
		{
			uu.Spew("clear vel: %d, no vphys objects", bClearVelocity);
		}
	});
	ORIG_CGrabController__DetachEntity(thisptr, edx, bClearVelocity);
});

BASIC_OFFSET_HOOK(server, 0x0193b30, penetrateevent_t&, __fastcall, CCollisionEvent__FindOrAddPenetrateEvent, (CCollisionEvent * thisptr, int edx, CBaseEntity* pEntity0, CBaseEntity* pEntity1), {
	// swap (this may make a difference somewhere else)
	if (pEntity0->GetRefEHandle().GetEntryIndex() > pEntity1->GetRefEHandle().GetEntryIndex())
		std::swap(pEntity0, pEntity1);
	penetrateevent_t& pEvent = ORIG_CCollisionEvent__FindOrAddPenetrateEvent(thisptr, edx, pEntity0, pEntity1);
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

BASIC_OFFSET_HOOK(engine, 0x485e0, void, __cdecl, _Host_RunFrame_Client, (bool framefinished), {
	URINATE_SIMPLE(true);
	ORIG__Host_RunFrame_Client(framefinished);
});

BASIC_OFFSET_HOOK(vphysics, 0x17b90, void, __fastcall, CPhysicsObject__AddVelocity, (CPhysicsObject * thisptr, int edx, Vector* vec, AngularImpulse* vecAng), {
	Vector inVecCopy = *vec;
	URINATE_WITH_INFO(true, {
		Vector actual;
		thisptr->GetVelocity(&actual, nullptr);
		uu.Spew("vel: " V_FMT ", add vel: " V_FMT, V_UNP(actual), V_UNP(inVecCopy));
	});
	ORIG_CPhysicsObject__AddVelocity(thisptr, edx, vec, vecAng);
});

BASIC_OFFSET_HOOK(vphysics, 0x129b0, void, __fastcall, CPhysicsEnvironment__Simulate, (CPhysicsEnvironment * thisptr, int edx, float deltatime), {
	URINATE_SIMPLE(true);
	ORIG_CPhysicsEnvironment__Simulate(thisptr, edx, deltatime);
});

BASIC_OFFSET_HOOK(server, 0xea710, void, __fastcall, CBaseEntity__VPhysicsUpdate, (CBaseEntity * thisptr, int edx, CPhysicsObject* pPhysics), {
	URINATE_WITH_INFO(true, {
		Vector vphysPos;
		pPhysics->GetPosition(&vphysPos, nullptr);
		uu.Spew("(%d) \"%s\", pos: " V_FMT ", vphys pos: " V_FMT, thisptr->GetRefEHandle().GetEntryIndex(), thisptr->GetClassname(), V_UNP(ENT_POS(thisptr)), V_UNP(vphysPos));
	});
	ORIG_CBaseEntity__VPhysicsUpdate(thisptr, edx, pPhysics);
});
