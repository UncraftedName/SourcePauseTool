#include "..\..\stdafx.hpp"
#pragma once

#include <SPTLib\IHookableNameFilter.hpp>
#include "..\..\utils\patterncontainer.hpp"
#include "engine\iserverplugin.h"
#include "tier3\tier3.h"

typedef struct Tri_t
{
	Vector v1, v2, v3;
};

typedef int(__fastcall* _CPhysicsCollision__CreateDebugMesh)(IPhysicsCollision* thisptr,
                                                             int dummy,
                                                             const CPhysCollide* pCollisionModel,
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

	bool IsCreateDebugMeshHooked();
	int CreateDebugMesh(const CPhysCollide* pCollisionModel, Tri_t** outTriangles); // returns triangle count
	void DestroyDebugMesh(Tri_t* triangles);

	bool* isgFlagPtr;

protected:
	PatternContainer patternContainer;
	_CPhysicsCollision__CreateDebugMesh ORIG_CPhysicsCollision__CreateDebugMesh;
};
