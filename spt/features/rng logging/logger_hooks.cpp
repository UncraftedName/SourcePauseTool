#include "stdafx.h"

#include "vphysics_interface.h"
#define GAME_DLL
#include "cbase.h"

#include "logger.hpp"
#include "signals.hpp"
#include "interfaces.hpp"
#include "..\ent_props.hpp"

class LoggerHooks : public FeatureWrapper<LoggerHooks>
{
public:
	std::vector<std::tuple<std::string, size_t, std::string, void**, void*>> queuedHooks;

	struct
	{
		int m_vecAbsOrigin;
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
}

void LoggerHooks::LoadFeature()
{
	FrameSignal.Connect(this, &LoggerHooks::OnFrameSignal);
	FinishRestoreSignal.Connect(this, &LoggerHooks::OnRestoreSignal);
	SetPausedSignal.Connect(this, &LoggerHooks::OnPauseSignal);

	offsets.m_vecAbsOrigin = spt_entprops.GetFieldOffset("CBaseEntity", "m_vecAbsOrigin", true);
}

void LoggerHooks::UnloadFeature()
{
	queuedHooks.clear();
}

void LoggerHooks::OnFrameSignal()
{
	URINATE_SIMPLE(false);
	/*edict_t* ed = interfaces::engine_server->PEntityOfEntIndex(606);
	if (ed && ed->GetIServerEntity())
	{
		CBaseEntity* pEntity = ed->GetIServerEntity()->GetBaseEntity();
		URINATE_WITH_INFO(false, { uu.Spew("ent: (%d) \"%s\", ent pos: " V_FMT, pEntity->GetRefEHandle().GetEntryIndex(), pEntity->GetClassname(), V_UNP(ENT_POS(pEntity))); });
	}
	else
	{
		URINATE_WITH_INFO(false, { uu.Spew("null ent"); });
	}*/
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
