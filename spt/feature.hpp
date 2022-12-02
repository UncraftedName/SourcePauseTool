#pragma once
#include "interface.h"
#include <array>
#include <vector>
#include <functional>
#include <unordered_map>
#include "SPTLib\patterns.hpp"
#include "SPTLib\memutils.hpp"
#include "convar.hpp"

#define DECL_MEMBER_CDECL(type, name, ...) \
	using _##name = type(__cdecl*)(__VA_ARGS__); \
	_##name ORIG_##name = nullptr;

#define DECL_HOOK_CDECL(type, name, ...) \
	DECL_MEMBER_CDECL(type, name, ##__VA_ARGS__) \
	static type __cdecl HOOKED_##name(__VA_ARGS__)

#define HOOK_CDECL(type, className, name, ...) type __cdecl className::HOOKED_##name(__VA_ARGS__)

#define DECL_MEMBER_THISCALL(type, name, ...) \
	using _##name = type(__fastcall*)(void* thisptr, int edx, ##__VA_ARGS__); \
	_##name ORIG_##name = nullptr;

#define DECL_HOOK_THISCALL(type, name, ...) \
	DECL_MEMBER_THISCALL(type, name, ##__VA_ARGS__) \
	static type __fastcall HOOKED_##name(void* thisptr, int edx, ##__VA_ARGS__)

#define HOOK_THISCALL(type, className, name, ...) \
	type __fastcall className::HOOKED_##name(void* thisptr, int edx, ##__VA_ARGS__)

#define ADD_RAW_HOOK(moduleName, name) \
	AddRawHook(#moduleName, reinterpret_cast<void**>(&ORIG_##name##), reinterpret_cast<void*>(HOOKED_##name##));
#define FIND_PATTERN(moduleName, name) \
	AddPatternHook(patterns::##name##, #moduleName, #name, reinterpret_cast<void**>(&ORIG_##name##), nullptr);
#define HOOK_FUNCTION(moduleName, name) \
	AddPatternHook(patterns::##name##, \
	               #moduleName, \
	               #name, \
	               reinterpret_cast<void**>(&ORIG_##name##), \
	               reinterpret_cast<void*>(HOOKED_##name##));
#define InitCommand(command) InitConcommandBase(command##_command)

using json = nlohmann::json;

struct VFTableHook
{
	VFTableHook(void** vftable, int index, void* functionToHook, void** origPtr);

	void** vftable;
	int index;
	void* functionToHook;
	void** origPtr;
};

struct PatternHook
{
	PatternHook(patterns::PatternWrapper* patternArr,
	            size_t size,
	            const char* patternName,
	            void** origPtr,
	            void* functionHook)
	{
		this->patternArr = patternArr;
		this->size = size;
		this->patternName = patternName;
		this->origPtr = origPtr;
		this->functionHook = functionHook;
	}

	patterns::PatternWrapper* patternArr;
	size_t size;
	const char* patternName;
	void** origPtr;
	void* functionHook;
	std::string md5Digest;
};

struct MatchAllPattern
{
	MatchAllPattern(patterns::PatternWrapper* patternArr,
	                size_t size,
	                const char* patternName,
	                std::vector<patterns::MatchedPattern>* foundVec)
	{
		this->patternArr = patternArr;
		this->size = size;
		this->patternName = patternName;
		this->foundVec = foundVec;
	}

	patterns::PatternWrapper* patternArr;
	size_t size;
	const char* patternName;
	std::vector<patterns::MatchedPattern>* foundVec;
};

struct OffsetHook
{
	int32_t offset;
	const char* patternName;
	void** origPtr;
	void* functionHook;
};

struct RawHook
{
	const char* patternName;
	void** origPtr;
	void* functionHook;
};

namespace pc // pattern cache
{
	struct PatternCacheEntry
	{
		std::string digest;
		size_t offset;
		size_t patternIdx;
	};

	inline void to_json(json& j, const PatternCacheEntry& p)
	{
		j = json{{"digest", p.digest}, {"offset", p.offset}, {"pattern index", p.patternIdx}};
	}

	inline void from_json(const json& j, PatternCacheEntry& p)
	{
		if (j.contains("digest") && j.contains("offset") && j.contains("pattern index"))
		{
			j.at("digest").get_to(p.digest);
			j.at("offset").get_to(p.offset);
			j.at("pattern index").get_to(p.patternIdx);
		}
	}

	struct PatternCacheModule
	{
		std::string digest;
		::std::unordered_map<::std::string, PatternCacheEntry> patterns;
	};

	inline void to_json(json& j, const PatternCacheModule& p)
	{
		j = {{"digest", p.digest}, {"patterns", p.patterns}};
	}

	inline void from_json(const json& j, PatternCacheModule& p)
	{
		if (j.contains("digest") && j.contains("patterns"))
		{
			j.at("digest").get_to(p.digest);
			j.at("patterns").get_to(p.patterns);
		}
	}

	struct PatternCache
	{
		// {module_name -> {pattern_name -> entry}}
		::std::unordered_map<::std::string, PatternCacheModule> modules;
		bool updated = false;

		bool empty()
		{
			return modules.empty();
		}
	};

	inline void to_json(json& j, const PatternCache& p)
	{
		j = {"pattern cache", p.modules};
	}

	inline void from_json(const json& j, PatternCache& p)
	{
		if (j.contains("pattern cache"))
			j.at("pattern cache").get_to(p.modules);
	}
} // namespace pc

struct ModuleHookData
{
	std::vector<PatternHook> patternHooks;
	std::vector<MatchAllPattern> matchAllPatterns;
	std::vector<VFTableHook> vftableHooks;
	std::vector<OffsetHook> offsetHooks;

	std::vector<std::pair<void**, void*>> funcPairs;
	std::vector<void**> hookedFunctions;
	std::vector<VFTableHook> existingVTableHooks;
	void InitModule(const std::wstring& moduleName, pc::PatternCache& cache);
	void HookModule(const std::wstring& moduleName);
	void UnhookModule(const std::wstring& moduleName);
};

class Feature
{
public:
	virtual ~Feature(){};
	virtual bool ShouldLoadFeature()
	{
		return true;
	};
	virtual void InitHooks(){};
	virtual void PreHook(){};
	virtual void LoadFeature(){};
	virtual void UnloadFeature(){};
	virtual Feature* CreateNewInstance() = 0;
	virtual void Move(Feature* instance) = 0;

	static void ReloadFeatures();
	static void LoadFeatures();
	static void UnloadFeatures();

	template<size_t PatternLength>
	static void AddPatternHook(const std::array<patterns::PatternWrapper, PatternLength>& patterns,
	                           std::string moduleName,
	                           const char* patternName,
	                           void** origPtr = nullptr,
	                           void* functionHook = nullptr);
	template<size_t PatternLength>
	static void AddMatchAllPattern(const std::array<patterns::PatternWrapper, PatternLength>& patterns,
	                               std::string moduleName,
	                               const char* patternName,
	                               std::vector<patterns::MatchedPattern>* foundVec);
	static void AddRawHook(std::string moduleName, void** origPtr, void* functionHook);
	static void AddPatternHook(PatternHook hook, std::string moduleEnum);
	static void AddMatchAllPattern(MatchAllPattern hook, std::string moduleName);
	static void AddVFTableHook(VFTableHook hook, std::string moduleEnum);
	static void AddOffsetHook(std::string moduleName,
	                          int offset,
	                          const char* patternName,
	                          void** origPtr = nullptr,
	                          void* functionHook = nullptr);
	static int GetPatternIndex(void** origPtr);

	Feature();

protected:
	void InitConcommandBase(ConCommandBase& convar);
	bool AddHudCallback(const char* sortKey, std::function<void()> func, ConVar& cvar);

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

template<size_t PatternLength>
inline void Feature::AddPatternHook(const std::array<patterns::PatternWrapper, PatternLength>& p,
                                    std::string moduleEnum,
                                    const char* patternName,
                                    void** origPtr,
                                    void* functionHook)
{
	AddPatternHook(PatternHook(const_cast<patterns::PatternWrapper*>(p.data()),
	                           PatternLength,
	                           patternName,
	                           origPtr,
	                           functionHook),
	               moduleEnum);
}

template<size_t PatternLength>
inline void Feature::AddMatchAllPattern(const std::array<patterns::PatternWrapper, PatternLength>& patterns,
                                        std::string moduleName,
                                        const char* patternName,
                                        std::vector<patterns::MatchedPattern>* foundVec)
{
	AddMatchAllPattern(MatchAllPattern(const_cast<patterns::PatternWrapper*>(patterns.data()),
	                                   PatternLength,
	                                   patternName,
	                                   foundVec),
	                   moduleName);
}
