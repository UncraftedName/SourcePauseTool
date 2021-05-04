#include "..\..\stdafx.hpp"
#pragma once

#include <SPTLib\IHookableNameFilter.hpp>
#include "..\..\utils\patterncontainer.hpp"
#include "engine\iserverplugin.h"
#include "tier3\tier3.h"

typedef int(__fastcall* _CPhysicsCollision__CreateDebugMesh)(IPhysicsCollision* thisptr,
                                                             int dummy,
                                                             CPhysCollide* pCollisionModel,
                                                             Vector** outVerts);

class VPhysicsDLL : public IHookableNameFilter
{
public:
	VPhysicsDLL() : IHookableNameFilter({L"vphysics.dll"}){};
	virtual void Hook(const std::wstring& moduleName,
	                  void* moduleHandle,
	                  void* moduleBase,
	                  size_t moduleLength,
	                  bool needToIntercept);

	virtual void Unhook();
	virtual void Clear();

	_CPhysicsCollision__CreateDebugMesh ORIG_CPhysicsCollision__CreateDebugMesh; // after using this, make sure to g_pMemAlloc->Free the verts

	bool* isgFlagPtr;

protected:
	PatternContainer patternContainer;
};
