#include "stdafx.h"
#include <future>
#include <fstream>
#include <libloaderapi.h>
#include <iomanip>
#include "convar.hpp"
#include "feature.hpp"
#include "interfaces.hpp"
#include "cvars.hpp"
#include "features\hud.hpp"
#include "SPTLib\sptlib.hpp"
#include "dbg.h"
#include "SPTLib\Windows\detoursutils.hpp"
#include "SPTLib\Hooks.hpp"
#include "cvars.hpp"
#include "spt\utils\file.hpp"
#include "thirdparty\md5.hpp"

static std::unordered_map<std::string, ModuleHookData> moduleHookData;
static std::unordered_map<uintptr_t, int> patternIndices;
static bool loadedOnce = false;
static bool reloadingFeatures = false;

static std::vector<Feature*>& GetFeatures()
{
	static std::vector<Feature*> features;
	return features;
}

void Feature::ReloadFeatures()
{
	reloadingFeatures = true;
	for (Feature* feature : GetFeatures())
	{
		auto instance = feature->CreateNewInstance();
		feature->Move(instance);
		delete instance;
	}
	reloadingFeatures = false;
}

void Feature::LoadFeatures()
{
	// This is a restart, reload the features
	if (loadedOnce)
	{
		ReloadFeatures();
	}

	Hooks::InitInterception(true);

	for (auto feature : GetFeatures())
	{
		if (!feature->moduleLoaded && feature->ShouldLoadFeature())
		{
			feature->startedLoading = true;
			feature->InitHooks();
		}
	}

	InitModules();

	for (auto feature : GetFeatures())
	{
		if (!feature->moduleLoaded && feature->startedLoading)
		{
			feature->PreHook();
		}
	}

	Hook();

	for (auto feature : GetFeatures())
	{
		if (!feature->moduleLoaded && feature->startedLoading)
		{
			feature->LoadFeature();
			feature->moduleLoaded = true;
		}
	}

	loadedOnce = true;
}

void Feature::UnloadFeatures()
{
	for (auto feature : GetFeatures())
	{
		if (feature->moduleLoaded)
		{
			feature->UnloadFeature();
			feature->moduleLoaded = false;
		}
	}

	Unhook();

	for (auto feature : GetFeatures())
	{
		if (feature->moduleLoaded)
		{
			feature->~Feature();
		}
	}

	moduleHookData.clear();
	patternIndices.clear();
}

void Feature::AddVFTableHook(VFTableHook hook, std::string moduleEnum)
{
	if (moduleHookData.find(moduleEnum) == moduleHookData.end())
	{
		moduleHookData[moduleEnum] = ModuleHookData();
	}

	auto& mhd = moduleHookData[moduleEnum];
	mhd.vftableHooks.push_back(hook);
}

Feature::Feature()
{
	moduleLoaded = false;
	startedLoading = false;
	if (!reloadingFeatures)
		GetFeatures().push_back(this);
}

void Feature::InitModules()
{
	pc::PatternCache cache;

	std::string cachePath = GetGameDir() + "\\spt_pattern_cache.json";
	std::fstream f(cachePath, std::ios::in | std::ios::out | std::ios::app);
	if (f)
	{
		f.close();
		f.open(cachePath, std::ios::in | std::ios::out);
		json jd = json::parse(f, nullptr, false);
		jd.get_to(cache);
		if (cache.empty())
			DevMsg("spt: pattern cache is empty!\n");
	}

	for (auto& pair : moduleHookData)
	{
		pair.second.InitModule(Convert(pair.first + ".dll"), cache);
	}

	if (f && cache.updated)
	{
		f.close();
		f.open(cachePath, std::ios::out | std::ios::trunc);
		f << std::setw(2) << json{cache};
	}
}

void Feature::Hook()
{
	for (auto& pair : moduleHookData)
	{
		pair.second.HookModule(Convert(pair.first + ".dll"));
	}
}

void Feature::Unhook()
{
	for (auto& pair : moduleHookData)
	{
		pair.second.UnhookModule(Convert(pair.first + ".dll"));
	}
}

void Feature::AddOffsetHook(std::string moduleEnum,
                            int offset,
                            const char* patternName,
                            void** origPtr,
                            void* functionHook)
{
	if (moduleHookData.find(moduleEnum) == moduleHookData.end())
	{
		moduleHookData[moduleEnum] = ModuleHookData();
	}

	auto& mhd = moduleHookData[moduleEnum];
	mhd.offsetHooks.push_back(OffsetHook{offset, patternName, origPtr, functionHook});
}

int Feature::GetPatternIndex(void** origPtr)
{
	uintptr_t ptr = reinterpret_cast<uintptr_t>(origPtr);
	if (patternIndices.find(ptr) != patternIndices.end())
	{
		return patternIndices[ptr];
	}
	else
	{
		return -1;
	}
}

void Feature::InitConcommandBase(ConCommandBase& convar)
{
	Cvar_InitConCommandBase(convar, this);
}

bool Feature::AddHudCallback(const char* sortKey, std::function<void()> func, ConVar& convar)
{
#ifdef SPT_HUD_ENABLED
	bool result = spt_hud.AddHudCallback(HudCallback(
	    sortKey, func, [&convar]() { return convar.GetBool(); }, false));

	if (result)
	{
		InitConcommandBase(convar);
	}

	return result;
#else
	return false;
#endif
}

void Feature::AddRawHook(std::string moduleName, void** origPtr, void* functionHook)
{
	if (moduleHookData.find(moduleName) == moduleHookData.end())
	{
		moduleHookData[moduleName] = ModuleHookData();
	}

	auto& hookData = moduleHookData[moduleName];
	hookData.funcPairs.emplace_back(origPtr, functionHook);
	hookData.hookedFunctions.emplace_back(origPtr);
}

void Feature::AddPatternHook(PatternHook hook, std::string moduleName)
{
	if (moduleHookData.find(moduleName) == moduleHookData.end())
	{
		moduleHookData[moduleName] = ModuleHookData();
	}

	auto& mhd = moduleHookData[moduleName];
	mhd.patternHooks.push_back(hook);
}

void Feature::AddMatchAllPattern(MatchAllPattern hook, std::string moduleName)
{
	if (moduleHookData.find(moduleName) == moduleHookData.end())
	{
		moduleHookData[moduleName] = ModuleHookData();
	}

	auto& mhd = moduleHookData[moduleName];
	mhd.matchAllPatterns.push_back(hook);
}

void ModuleHookData::UnhookModule(const std::wstring& moduleName)
{
	if (!hookedFunctions.empty())
		DetoursUtils::DetachDetours(moduleName, hookedFunctions.size(), &hookedFunctions[0]);

	for (auto& vft_hook : existingVTableHooks)
		MemUtils::HookVTable(vft_hook.vftable, vft_hook.index, *vft_hook.origPtr);
}

void ModuleHookData::InitModule(const std::wstring& moduleName, pc::PatternCache& cache)
{
	void* handle;
	void* moduleStart;
	size_t moduleSize;

	std::string moduleNameA = Convert(moduleName);

	if (MemUtils::GetModuleInfo(moduleName, &handle, &moduleStart, &moduleSize))
	{
		DevMsg("Hooking %s (start: %p; size: %x)...\n", moduleNameA.c_str(), moduleStart, moduleSize);
	}
	else
	{
		DevMsg("Couldn't hook %s, not loaded\n", moduleNameA.c_str());
		return;
	}

	pc::PatternCacheModule& cachedModule = cache.modules[moduleNameA];
	MD5 md5{};

	char modulePath[MAX_PATH + 1];
	GetModuleFileNameA(reinterpret_cast<HMODULE>(handle), modulePath, sizeof modulePath);
	std::fstream f(modulePath, std::ios::in | std::ios::binary);
	if (f)
	{
		// f.rdbuf()->pubsetbuf(0, 0);
		char buf[1024 * 16];
		while (!f.eof())
		{
			f.read(buf, sizeof buf);
			md5.update(buf, f.gcount());
		}
		md5.finalize();
		f.close();
	}

	if (cachedModule.digest != md5.hexdigest())
	{
		DevMsg("[%s] Pattern cache doesn't match for module, clearing...\n", moduleNameA.c_str());
		cachedModule.digest = md5.hexdigest();
		cachedModule.patterns.clear();
		cache.updated = true;
	}

	std::vector<std::future<patterns::PatternWrapper*>> hooks;
	std::vector<std::future<std::vector<patterns::MatchedPattern>>> mhooks;
	hooks.reserve(patternHooks.size());

	for (auto& mpattern : matchAllPatterns)
	{
		mhooks.emplace_back(MemUtils::find_all_sequences_async(moduleStart,
		                                                       moduleSize,
		                                                       mpattern.patternArr,
		                                                       mpattern.patternArr + mpattern.size));
	}

	std::vector<size_t> patternIdxRef;

	for (auto& pattern : patternHooks)
	{
		md5 = MD5{};
		for (size_t i = 0; i < pattern.size; i++)
		{
			const auto& pw = pattern.patternArr[i];
			md5.update(pw.bytes(), pw.length());
		}
		md5.finalize();
		pattern.md5Digest = md5.hexdigest();

		auto it = cachedModule.patterns.find(pattern.patternName);
		bool cachedPatternValid = it != cachedModule.patterns.end();
		if (cachedPatternValid)
		{
			// digest matches
			cachedPatternValid = it->second.digest == pattern.md5Digest;
		}
		if (cachedPatternValid)
		{
			// the pattern index makes sense
			cachedPatternValid = it->second.patternIdx < pattern.size;
		}
		if (cachedPatternValid)
		{
			// the corresponding pattern is somewhere inside the module
			const auto& pw = pattern.patternArr[it->second.patternIdx];

			cachedPatternValid = UINT32_MAX - pw.length() > it->second.offset
			                     && it->second.offset + pw.length() < moduleSize;
		}
		if (cachedPatternValid)
		{
			// the pattern actually matches at the cached offset
			const auto& pw = pattern.patternArr[it->second.patternIdx];
			cachedPatternValid = pw.match((uchar*)moduleStart + it->second.offset);
		}
		if (cachedPatternValid)
		{
			DevMsg("[%s] Using cached offset %p for pattern %s", it->second.offset, pattern.patternName);
			*pattern.origPtr = reinterpret_cast<char*>(moduleStart) + it->second.offset;

			if (pattern.functionHook)
			{
				funcPairs.emplace_back(pattern.origPtr, pattern.functionHook);
				hookedFunctions.emplace_back(pattern.origPtr);
			}
		}
		else
		{
			patternIdxRef.push_back(&pattern - patternHooks.data());
			cache.updated = true;
			if (it != cachedModule.patterns.end())
				cachedModule.patterns.erase(it);
			hooks.emplace_back(MemUtils::find_unique_sequence_async(*pattern.origPtr,
			                                                        moduleStart,
			                                                        moduleSize,
			                                                        pattern.patternArr,
			                                                        pattern.patternArr + pattern.size));
		}
	}

	funcPairs.reserve(funcPairs.size() + patternHooks.size());
	hookedFunctions.reserve(hookedFunctions.size() + patternHooks.size());

	for (std::size_t i = 0; i < mhooks.size(); ++i)
	{
		auto modulePattern = matchAllPatterns[i];
		*modulePattern.foundVec = std::move(mhooks[i].get());
		DevMsg("[%s] Found %u instances of pattern %s\n",
		       moduleNameA.c_str(),
		       modulePattern.foundVec->size(),
		       modulePattern.patternName);
	}

	for (std::size_t i = 0; i < hooks.size(); ++i)
	{
		auto foundPattern = hooks[i].get();
		auto modulePattern = patternHooks[patternIdxRef[i]];

		if (*modulePattern.origPtr)
		{
			if (modulePattern.functionHook)
			{
				funcPairs.emplace_back(modulePattern.origPtr, modulePattern.functionHook);
				hookedFunctions.emplace_back(modulePattern.origPtr);
			}

			DevMsg("[%s] Found %s at %p (using the %s pattern).\n",
			       moduleNameA.c_str(),
			       modulePattern.patternName,
			       *modulePattern.origPtr,
			       foundPattern->name());
			patternIndices[reinterpret_cast<uintptr_t>(modulePattern.origPtr)] =
			    foundPattern - modulePattern.patternArr;

			cachedModule.patterns.emplace(std::piecewise_construct,
			                              std::forward_as_tuple(modulePattern.patternName),
			                              std::forward_as_tuple(modulePattern.md5Digest,
			                                                    (size_t)*modulePattern.origPtr
			                                                        - (size_t)moduleStart,
			                                                    foundPattern - modulePattern.patternArr));
		}
		else
		{
			DevWarning("[%s] Could not find %s.\n", moduleNameA.c_str(), modulePattern.patternName);
		}
	}

	for (auto& offset : offsetHooks)
	{
		*offset.origPtr = reinterpret_cast<char*>(moduleStart) + offset.offset;

		DevMsg("[%s] Found %s at %p via a fixed offset.\n",
		       moduleNameA.c_str(),
		       offset.patternName,
		       *offset.origPtr);

		if (offset.functionHook)
		{
			funcPairs.emplace_back(offset.origPtr, offset.functionHook);
			hookedFunctions.emplace_back(offset.origPtr);
		}
	}
}

void ModuleHookData::HookModule(const std::wstring& moduleName)
{
	if (!vftableHooks.empty())
	{
		for (auto& vft_hook : vftableHooks)
		{
			*vft_hook.origPtr = vft_hook.vftable[vft_hook.index];
			MemUtils::HookVTable(vft_hook.vftable, vft_hook.index, vft_hook.functionToHook);
		}
	}

	if (!funcPairs.empty())
	{
		for (auto& entry : funcPairs)
			MemUtils::MarkAsExecutable(*(entry.first));

		DetoursUtils::AttachDetours(moduleName, funcPairs.size(), &funcPairs[0]);
	}

	// Clear any hooks that were added
	offsetHooks.clear();
	patternHooks.clear();
	// VTable hooks have to be stored for the unhooking code
	existingVTableHooks.insert(existingVTableHooks.end(), vftableHooks.begin(), vftableHooks.end());
	vftableHooks.clear();
}

VFTableHook::VFTableHook(void** vftable, int index, void* functionToHook, void** origPtr)
{
	this->vftable = vftable;
	this->index = index;
	this->functionToHook = functionToHook;
	this->origPtr = origPtr;
}
