#pragma once

// Include real Lua 5.3 headers first
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// Now block sol2's compat layer by defining its include guard
// Check the actual guard name in your sol2 copy first:
#ifndef SOL_COMPAT_53_H
#define SOL_COMPAT_53_H
#endif