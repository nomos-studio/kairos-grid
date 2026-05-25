/*
 * kairos-grid compat shim — shadows surge/src/common/globals.h.
 *
 * The original asserts __cplusplus == 201703L, which blocks C++20+.
 * This shadow relaxes that to >= 201703L.  All other content is verbatim
 * from the surge commit vendored by libs/surge/CMakeLists.txt (304f6dc).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SURGE_SRC_COMMON_GLOBALS_H
#define SURGE_SRC_COMMON_GLOBALS_H

#include <math.h>
#include <stdio.h>
#include <assert.h>
#include <algorithm>
#include <string>

// Relaxed from == 201703L: kairos-grid requires C++20; surge requires C++17+.
static_assert(__cplusplus >= 201703L, "Surge requires C++17 or later");

#if MAC

#if defined(__x86_64__)
#else
#define ARM_NEON 1
#endif

#endif

#if LINUX
#if defined(__aarch64__) || defined(__arm__)
#define ARM_NEON 1
#endif
#endif

#if defined(__SSE2__) || defined(_M_AMD64) || defined(_M_X64) ||                                   \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#else
#define SIMDE_ENABLE_NATIVE_ALIASES
#include "simde/x86/sse2.h"
#endif

#if MAC || LINUX
#include <strings.h>

static inline int _stricmp(const char *s1, const char *s2) { return strcasecmp(s1, s2); }
#endif

#define _SURGE_STR(x) #x
#define SURGE_STR(x) _SURGE_STR(x)

#if !defined(SURGE_COMPILE_BLOCK_SIZE)
#error You must compile with -DSURGE_COMPILE_BLOCK_SIZE=32 (or whatnot)
#endif

const int BASE_WINDOW_SIZE_X = 913;
const int BASE_WINDOW_SIZE_Y = 569;
const int NAMECHARS = 64;
const int BLOCK_SIZE = SURGE_COMPILE_BLOCK_SIZE;
const int OSC_OVERSAMPLING = 2;
const int BLOCK_SIZE_OS = OSC_OVERSAMPLING * BLOCK_SIZE;
const int BLOCK_SIZE_QUAD = BLOCK_SIZE >> 2;
const int BLOCK_SIZE_OS_QUAD = BLOCK_SIZE_OS >> 2;
const int OB_LENGTH = BLOCK_SIZE_OS << 1;
const int OB_LENGTH_QUAD = OB_LENGTH >> 2;
const float BLOCK_SIZE_INV = (1.f / BLOCK_SIZE);
const float BLOCK_SIZE_OS_INV = (1.f / BLOCK_SIZE_OS);
const int MAX_FB_COMB = 2048;
const int MAX_FB_COMB_EXTENDED = 2048 * 64;
const int MAX_VOICES = 64;
const int MAX_UNISON = 16;
const int N_OUTPUTS = 2;
const int N_INPUTS = 2;

const int DEFAULT_POLYLIMIT = 16;

const int DEFAULT_OSC_PORT_IN = 53280;
const int DEFAULT_OSC_PORT_OUT = 53281;
const std::string DEFAULT_OSC_IPADDR_OUT = "127.0.0.1";
#endif // SURGE_SRC_COMMON_GLOBALS_H
