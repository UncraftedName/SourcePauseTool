#pragma once

// some hacks to make this json library behave well with its disgusting macros

#ifdef null
#undef null
#endif

#include "json.hpp"

#undef and
#undef or
