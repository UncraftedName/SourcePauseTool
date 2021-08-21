#include "stdafx.h"

#include "vphysicsDLL.hpp"

#include "convar.h"

#include "..\modules.hpp"
#include "..\patterns.hpp"

#define DEF_FUTURE(name) auto f##name = FindAsync(ORIG_##name, patterns::vphysics::##name);
#define GET_HOOKEDFUTURE(future_name) \
	{ \
		auto pattern = f##future_name.get(); \
		if (ORIG_##future_name) \
		{ \
			DevMsg("[vphysics dll] Found " #future_name " at %p (using the %s pattern).\n", \
			       ORIG_##future_name, \
			       pattern->name()); \
			patternContainer.AddHook(HOOKED_##future_name, (PVOID*)&ORIG_##future_name); \
			for (int i = 0; true; ++i) \
			{ \
				if (patterns::vphysics::##future_name.at(i).name() == pattern->name()) \
				{ \
					patternContainer.AddIndex((PVOID*)&ORIG_##future_name, i, pattern->name()); \
					break; \
				} \
			} \
		} \
		else \
		{ \
			DevWarning("[vphysics dll] Could not find " #future_name ".\n"); \
		} \
	}

#define GET_FUTURE(future_name) \
	{ \
		auto pattern = f##future_name.get(); \
		if (ORIG_##future_name) \
		{ \
			DevMsg("[vphysics dll] Found " #future_name " at %p (using the %s pattern).\n", \
			       ORIG_##future_name, \
			       pattern->name()); \
			for (int i = 0; true; ++i) \
			{ \
				if (patterns::vphysics::##future_name.at(i).name() == pattern->name()) \
				{ \
					patternContainer.AddIndex((PVOID*)&ORIG_##future_name, i, pattern->name()); \
					break; \
				} \
			} \
		} \
		else \
		{ \
			DevWarning("[vphysics dll] Could not find " #future_name ".\n"); \
		} \
	}

void VPhysicsDLL::Hook(const std::wstring& moduleName,
                       void* moduleHandle,
                       void* moduleBase,
                       size_t moduleLength,
                       bool needToIntercept)
{
	Clear();
	m_Name = moduleName;
	m_Base = moduleBase;
	m_Length = moduleLength;
	patternContainer.Init(moduleName);

	uint32_t ORIG_MiddleOfRecheck_ov_element = NULL;

	DEF_FUTURE(MiddleOfRecheck_ov_element);
	DEF_FUTURE(CPhysicsCollision__CreateDebugMesh);
	DEF_FUTURE(CPhysicsObject__GetPosition);

	GET_FUTURE(MiddleOfRecheck_ov_element);
	GET_HOOKEDFUTURE(CPhysicsCollision__CreateDebugMesh);
	GET_FUTURE(CPhysicsObject__GetPosition);

	if (ORIG_MiddleOfRecheck_ov_element)
		this->isgFlagPtr = *(bool**)(ORIG_MiddleOfRecheck_ov_element + 2);
	else
		Warning("y_spt_hud_isg 1 and y_spt_set_isg have no effect\n");

	patternContainer.Hook();
}

void VPhysicsDLL::Unhook()
{
	patternContainer.Unhook();
	Clear();
}

void VPhysicsDLL::Clear()
{
	IHookableNameFilter::Clear();
	ORIG_CPhysicsObject__GetPosition = nullptr;
	ORIG_CPhysicsCollision__CreateDebugMesh = nullptr;
	isgFlagPtr = nullptr;
	adjustDebugMesh = false;
}

int __fastcall VPhysicsDLL::HOOKED_CPhysicsCollision__CreateDebugMesh(IPhysicsCollision* thisptr,
                                                         int dummy,
                                                         const CPhysCollide* pCollisionModel,
                                                         Vector** outVerts)
{
	int vertCount = vphysicsDLL.ORIG_CPhysicsCollision__CreateDebugMesh(thisptr, dummy, pCollisionModel, outVerts);
	if (vphysicsDLL.adjustDebugMesh)
	{
		Vector* v = *outVerts;
		const Vector* player_origin = (Vector*)(GetServerPlayer() + 179);
		// determine the furthest vert from the player
		float maxDistSqr = 0;
		for (int i = 0; i < vertCount; i++)
			maxDistSqr = MAX(maxDistSqr, player_origin->DistToSqr(v[i]));
		float normEps = MAX(pow(maxDistSqr, 0.6) / 50000, 0.0001);
		// expand the debug mesh by some epsilon - we want the mesh to look good up close but prevent z fighting when it's far
		for (int i = 0; i < vertCount; i += 3)
		{
			Vector norm = (v[i] - v[i + 1]).Cross(v[i + 2] - v[i]);
			norm.NormalizeInPlace();
			norm *= normEps;
			v[i] += norm;
			v[i + 1] += norm;
			v[i + 2] += norm;
		}
	}
	return vertCount;
}
