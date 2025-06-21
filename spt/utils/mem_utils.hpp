#pragma once

#include <future>
#include <string>
#include <vector>
#include <array>

#include "SPTLib\patterns.hpp"
#include "SPTLib\MemUtils.hpp"

/*
* Prefer using DECL_STATIC_HOOK_XXX over DECL_HOOK_XXX.
* The difference is that with the former you can do:
* 
* IMPL_HOOK_XXX(SptFeature, void, GameFn, int arg1) {
*   ORIG_GameFn(arg1);
* }
* 
* The DECL_HOOK_XXX macros keep the ORIG pointer as a
* member, so you're forced to do:
* 
* IMPL_HOOK_XXX(SptFeature, void, GameFn, int arg1) {
*   spt_feature_instance.ORIG_GameFn(arg1);
* }
*/

#define _DECL_FN(qualifiers, call_conv, retType, name, ...) \
	using _##name = retType(call_conv*)(##__VA_ARGS__); \
	qualifiers _##name ORIG_##name = nullptr

#define _DECL_MEMBER_FN(call_conv, retType, name, ...) _DECL_FN(, call_conv, retType, name, ##__VA_ARGS__)

#define _DECL_HOOK_FN(call_conv, retType, name, ...) \
	_DECL_MEMBER_FN(call_conv, retType, name, ##__VA_ARGS__); \
	static retType call_conv HOOKED_##name(__VA_ARGS__)

#define _DECL_STATIC_FN(call_conv, retType, name, ...) _DECL_FN(inline static, call_conv, retType, name, ##__VA_ARGS__)

#define _DECL_STATIC_HOOK_FN(call_conv, retType, name, ...) \
	_DECL_STATIC_FN(call_conv, retType, name, ##__VA_ARGS__); \
	static retType call_conv HOOKED_##name(__VA_ARGS__)

#define _IMPL_HOOK_FN(call_conv, spt_class, retType, name, ...) \
	retType call_conv spt_class::HOOKED_##name(##__VA_ARGS__)

// cdecl

#define DECL_MEMBER_CDECL(retType, name, ...) _DECL_MEMBER_FN(__cdecl, retType, name, ##__VA_ARGS__)
#define DECL_HOOK_CDECL(retType, name, ...) _DECL_HOOK_FN(__cdecl, retType, name, ##__VA_ARGS__)
#define DECL_STATIC_CDECL(retType, name, ...) _DECL_STATIC_FN(__cdecl, retType, name, ##__VA_ARGS__)
#define DECL_STATIC_HOOK_CDECL(retType, name, ...) _DECL_STATIC_HOOK_FN(__cdecl, retType, name, ##__VA_ARGS__)
#define IMPL_HOOK_CDECL(spt_class, retType, name, ...) _IMPL_HOOK_FN(__cdecl, spt_class, retType, name, ##__VA_ARGS__)

// fastcall

#define DECL_MEMBER_FASTCALL(retType, name, ...) _DECL_MEMBER_FN(__fastcall, retType, name, ##__VA_ARGS__)
#define DECL_HOOK_FASTCALL(retType, name, ...) _DECL_HOOK_FN(__fastcall, retType, name, ##__VA_ARGS__)
#define DECL_STATIC_FASTCALL(retType, name, ...) _DECL_STATIC_FN(__fastcall, retType, name, ##__VA_ARGS__)
#define DECL_STATIC_HOOK_FASTCALL(retType, name, ...) _DECL_STATIC_HOOK_FN(__fastcall, retType, name, ##__VA_ARGS__)
#define IMPL_HOOK_FASTCALL(spt_class, retType, name, ...) \
	_IMPL_HOOK_FN(__fastcall, spt_class, retType, name, ##__VA_ARGS__)

// stdcall

#define DECL_MEMBER_STDCALL(retType, name, ...) _DECL_MEMBER_FN(__stdcall, retType, name, ##__VA_ARGS__)
#define DECL_HOOK_STDCALL(retType, name, ...) _DECL_HOOK_FN(__stdcall, retType, name, ##__VA_ARGS__)
#define DECL_STATIC_STDCALL(retType, name, ...) _DECL_STATIC_FN(__stdcall, retType, name, ##__VA_ARGS__)
#define DECL_STATIC_HOOK_STDCALL(retType, name, ...) _DECL_STATIC_HOOK_FN(__stdcall, retType, name, ##__VA_ARGS__)
#define IMPL_HOOK_STDCALL(spt_class, retType, name, ...) \
	_IMPL_HOOK_FN(__stdcall, spt_class, retType, name, ##__VA_ARGS__)

// thiscall convention - msvc doesn't allow a static function to be thiscall, we make the ORIG function __thiscall
// & the static __fastcall with a hidden edx param (the callee is allowed to clobber edx)

#define DECL_MEMBER_THISCALL(retType, name, thisType, ...) \
	_DECL_MEMBER_FN(__thiscall, retType, name, thisType thisptr, ##__VA_ARGS__)

#define DECL_HOOK_THISCALL(retType, name, thisType, ...) \
	DECL_MEMBER_THISCALL(retType, name, thisType, ##__VA_ARGS__); \
	static retType __fastcall HOOKED_##name(thisType thisptr, int _edx, ##__VA_ARGS__)

#define DECL_STATIC_THISCALL(retType, name, thisType, ...) \
	_DECL_STATIC_FN(__thiscall, retType, name, thisType thisptr, ##__VA_ARGS__)

#define DECL_STATIC_HOOK_THISCALL(retType, name, thisType, ...) \
	DECL_STATIC_THISCALL(retType, name, thisType, ##__VA_ARGS__); \
	static retType __fastcall HOOKED_##name(thisType thisptr, int _edx, ##__VA_ARGS__)

#define IMPL_HOOK_THISCALL(spt_class, retType, name, thisType, ...) \
	_IMPL_HOOK_FN(__fastcall, spt_class, retType, name, thisType thisptr, int _edx, ##__VA_ARGS__)

// misc

#define ADD_RAW_HOOK(moduleName, name) \
	AddRawHook(#moduleName, reinterpret_cast<void**>(&ORIG_##name##), reinterpret_cast<void*>(HOOKED_##name##));
#define HOOK_FUNCTION(moduleName, name) \
	AddPatternHook(patterns::##name##, \
	               #moduleName, \
	               #name, \
	               reinterpret_cast<void**>(&ORIG_##name##), \
	               reinterpret_cast<void*>(HOOKED_##name##));

#define FIND_PATTERN(moduleName, name) \
	AddPatternHook(patterns::##name##, #moduleName, #name, reinterpret_cast<void**>(&ORIG_##name##), nullptr);
#define FIND_PATTERN_ALL(moduleName, name) \
	AddMatchAllPattern(patterns::##name##, #moduleName, #name, &MATCHES_##name##);

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
};

struct VFTableHook
{
	VFTableHook(void** vftable, int index, void* functionToHook, void** origPtr);

	void** vftable;
	int index;
	void* functionToHook;
	void** origPtr;
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

void AddRawHook(std::string moduleName, void** origPtr, void* functionHook);
void AddPatternHook(PatternHook hook, std::string moduleEnum);
void AddMatchAllPattern(MatchAllPattern hook, std::string moduleName);
void AddVFTableHook(VFTableHook hook, std::string moduleEnum);
void AddOffsetHook(std::string moduleName,
                   int offset,
                   const char* patternName,
                   void** origPtr = nullptr,
                   void* functionHook = nullptr);
int GetPatternIndex(void** origPtr);

template<size_t PatternLength>
void AddMatchAllPattern(const std::array<patterns::PatternWrapper, PatternLength>& patterns,
                        std::string moduleName,
                        const char* patternName,
                        std::vector<patterns::MatchedPattern>* foundVec);

template<size_t PatternLength>
inline void AddPatternHook(const std::array<patterns::PatternWrapper, PatternLength>& p,
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
inline void AddMatchAllPattern(const std::array<patterns::PatternWrapper, PatternLength>& patterns,
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

// direct byte replacements is only needed in very niche applications and quite dangerous,
// so all of this should stay as macros and a pain in the arse to use

#define DECL_BYTE_REPLACE(name, size, ...) \
	uintptr_t PTR_##name = NULL; \
	byte ORIG_BYTES_##name[size] = {}; \
	byte NEW_BYTES_##name[size] = {##__VA_ARGS__};
#define INIT_BYTE_REPLACE(name, ptr) \
	PTR_##name = ptr; \
	if (ptr != NULL) \
	memcpy((void*)ORIG_BYTES_##name, (void*)(ptr), sizeof(ORIG_BYTES_##name))
#define DO_BYTE_REPLACE(name) \
	if (PTR_##name != NULL) \
		MemUtils::ReplaceBytes((void*)PTR_##name, sizeof(ORIG_BYTES_##name), NEW_BYTES_##name);
#define UNDO_BYTE_REPLACE(name) \
	if (PTR_##name != NULL) \
		MemUtils::ReplaceBytes((void*)PTR_##name, sizeof(ORIG_BYTES_##name), ORIG_BYTES_##name);
#define DESTROY_BYTE_REPLACE(name) \
	if (PTR_##name != NULL) \
	{ \
		UNDO_BYTE_REPLACE(name); \
		PTR_##name = NULL; \
		memset((void*)ORIG_BYTES_##name, 0x00, sizeof(ORIG_BYTES_##name)); \
	}
