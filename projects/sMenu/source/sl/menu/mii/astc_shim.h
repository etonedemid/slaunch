/* What Mesa's texcompress_astc.cpp needs from Mesa's own headers, so the
 * file (MIT, Philip Taylor / AMD) can be built here as it is. */
#pragma once
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define likely(x) __builtin_expect(!!(x), 1)
#define UNREACHABLE(msg) __builtin_unreachable()
/* HDR output only; this build always asks for unorm8. */
#define FP16_ONE 0x3C00
#define FP16_ZERO 0
static inline uint16_t _mesa_uint16_div_64k_to_half(uint16_t v) { (void)v; return 0; }
void sl_astc4x4_decode(uint8_t *dst, const uint8_t *src, unsigned width, unsigned height, bool srgb);
