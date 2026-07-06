#pragma once
#include <switch.h>

// R_TRY is provided by Atmosphere/libstratosphere (used by sSystem) but not by
// plain libnx (used by sMenu / sInstaller). Provide a portable fallback when it
// is not already defined, so the shared sCommon code compiles on both sides.
#ifndef R_TRY
#define R_TRY(_expr) do { const ::Result _sl_rc = (_expr); if (R_FAILED(_sl_rc)) return _sl_rc; } while (0)
#endif
