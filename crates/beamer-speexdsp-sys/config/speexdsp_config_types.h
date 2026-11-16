/* What speexdsp's configure generates from include/speex/speexdsp_config_types.h.in, for the
   builds that reach the last branch of speexdsp_types.h: Linux. macOS and Windows have branches
   of their own there and never include this file. build.rs puts this directory on the include
   path, so the vendored tree stays as released. */
#ifndef __SPEEX_TYPES_H__
#define __SPEEX_TYPES_H__

#include <stdint.h>

typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;

#endif
