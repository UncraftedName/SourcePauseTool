#include "..\..\stdafx.hpp"
#pragma once

#include <SPTLib\IHookableNameFilter.hpp>
#include "..\..\utils\patterncontainer.hpp"
#include "..\vphysics_interface.h"

class Vector;
typedef Vector AngularImpulse;

#define TYPEDEF_THISCALL(name, ret, ...) typedef ret(__fastcall* _##name)(void* thisptr, int edx, __VA_ARGS__)

typedef void(__fastcall* _CPhysicsObject__GetVelocity)(IPhysicsObject* thisptr, int edx, Vector* velocity, AngularImpulse* angularVelocity);

//TYPEDEF_THISCALL(CPhysicsObject__GetVelocity, void, Vector* velocity, AngularImpulse* angularVelocity);

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

	bool* isgFlagPtr;
	void** CPhysicsObjectVTable;
	_CPhysicsObject__GetVelocity CPhysicsObject__GetVelocity;

protected:
	PatternContainer patternContainer;
	void* ORIG_CreatePhysicsObject;
};
