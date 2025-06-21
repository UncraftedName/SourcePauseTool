#pragma once

#include <future>
#include <string>
#include <vector>
#include <array>

#include "SPTLib\MemUtils.hpp"

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
