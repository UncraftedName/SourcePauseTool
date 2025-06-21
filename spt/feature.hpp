#pragma once

#include <array>
#include <vector>
#include <functional>
#include <stdexcept>
#include <algorithm>

#include "mem_utils.hpp"

#include "convar.h"

#define InitCommand(command) InitConcommandBase(command##_command)

class Feature
{
public:
	virtual ~Feature() {};
	virtual bool ShouldLoadFeature()
	{
		return true;
	};
	virtual void InitHooks() {};
	virtual void PreHook() {};
	virtual void LoadFeature() {};
	virtual void UnloadFeature() {};
	virtual Feature* CreateNewInstance() = 0;
	virtual void Move(Feature* instance) = 0;

	static void ReloadFeatures();
	static void LoadFeatures();
	static void UnloadFeatures();

	Feature();

protected:
	void InitConcommandBase(ConCommandBase& convar);
	bool AddHudCallback(const char* key, std::function<void(std::string)> func, ConVar& cvar);

	bool moduleLoaded;
	bool startedLoading;

private:
	static void InitModules();
	static void Hook();
	static void Unhook();
};

template<typename T>
class FeatureWrapper : public Feature
{
public:
	virtual Feature* CreateNewInstance()
	{
		return new T();
	}

	virtual void Move(Feature* instance)
	{
		*((T*)this) = std::move(*(T*)instance);
	}
};
