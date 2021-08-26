#include "..\..\stdafx.hpp"
#pragma once

#include <SPTLib\IHookableNameFilter.hpp>
#include "..\..\utils\patterncontainer.hpp"
#include "engine\iserverplugin.h"
#include "tier3\tier3.h"

typedef int(__fastcall* _CPhysicsCollision__CreateDebugMesh)(const IPhysicsCollision* thisptr,
                                                             int dummy,
                                                             const CPhysCollide* pCollisionModel,
                                                             Vector** outVerts);
typedef void(__fastcall* _CPhysicsObject__GetPosition)(const void* thisptr, int dummy, Vector* worldPosition, QAngle* angles);

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

	_CPhysicsCollision__CreateDebugMesh ORIG_CPhysicsCollision__CreateDebugMesh;
	_CPhysicsCollision__CreateDebugMesh CPhysicsCollision__CreateDebugMesh_Func;
	_CPhysicsObject__GetPosition ORIG_CPhysicsObject__GetPosition;

	bool* isgFlagPtr;

protected:
	PatternContainer patternContainer;

	static int __fastcall HOOKED_CPhysicsCollision__CreateDebugMesh(const IPhysicsCollision* thisptr,
	                                                                int dummy,
	                                                                const CPhysCollide* pCollisionModel,
	                                                                Vector** outVerts);
};
