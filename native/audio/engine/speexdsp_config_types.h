/* SPDX-License-Identifier: GPL-3.0-only */
/* What speexdsp's configure generates from include/speex/speexdsp_config_types.h.in, for the
   non-Windows builds of the vendored echo canceller (third_party/speexdsp-1.2.1; Windows uses the
   _WIN32 branch of speexdsp_types.h and never includes this file). Found through the engine's
   include path. */
#ifndef __SPEEX_TYPES_H__
#define __SPEEX_TYPES_H__

#include <stdint.h>

typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;

#endif
