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

#define IN_MODULE(x) ((uint32_t)x > (uint32_t)moduleBase && (uint32_t)x < (uint32_t)moduleBase + (uint32_t)moduleLength)

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
	GET_FUTURE(MiddleOfRecheck_ov_element);
	DEF_FUTURE(CreatePhysicsObject);
	GET_FUTURE(CreatePhysicsObject);

	if (ORIG_MiddleOfRecheck_ov_element)
		isgFlagPtr = *(bool**)(ORIG_MiddleOfRecheck_ov_element + 2);
	else
		Warning("y_spt_hud_isg 1 and y_spt_set_isg have no effect\n");

	if (ORIG_CreatePhysicsObject)
	{
		#define GET_OFFSET(x) ((uint32_t)x - (uint32_t)moduleBase)

		CPhysicsObjectVTable = *(void***)((uint32_t)ORIG_CreatePhysicsObject + 670);
		
		Msg("ORIG_CreatePhysicsObject at vphysics.dll+0x%p\n", GET_OFFSET(ORIG_CreatePhysicsObject));
		Msg("vtable at vphysics.dll+0x%p\n", GET_OFFSET(CPhysicsObjectVTable));
		// vtable address is in dll, vtable is in dll, and first function is in dll
		if (!(IN_MODULE(CPhysicsObjectVTable) && IN_MODULE(*CPhysicsObjectVTable)))
			Warning("vtable for cphysicsobject not found\n");
	}

	if (CPhysicsObjectVTable)
	{
		CPhysicsObject__GetVelocity = _CPhysicsObject__GetVelocity(*(CPhysicsObjectVTable + 51));
		Msg("*CPhysicsObject__GetVelocity at vphysics.dll+0x%p\n", GET_OFFSET(CPhysicsObjectVTable + 51));
		Msg("CPhysicsObject__GetVelocity at vphysics.dll+0x%p\n", GET_OFFSET(CPhysicsObject__GetVelocity));
		Msg("CPhysicsObject__GetVelocity2 at vphysics.dll+0x%p\n",
		    GET_OFFSET (* (_CPhysicsObject__GetVelocity*)(CPhysicsObjectVTable + 51)));
	}
	else
	{
		Warning("CPhysics Object get vel not found\n");
	}

	patternContainer.Hook();
}

void VPhysicsDLL::Unhook()
{
	patternContainer.Unhook();
}

void VPhysicsDLL::Clear()
{
	isgFlagPtr = nullptr;
	ORIG_CreatePhysicsObject = nullptr;
	CPhysicsObjectVTable = nullptr;
	CPhysicsObject__GetVelocity = nullptr;
}
