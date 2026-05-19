/*
 * AdventOS stub — inttypes.h. Provides the int8_t/int16_t/int32_t/
 * int64_t + unsigned counterparts. Re-includes adventos-libc.h so
 * size_t/uintptr_t/etc. are also visible.
 */
#ifndef _ADVENTOS_STUB_INTTYPES_H
#define _ADVENTOS_STUB_INTTYPES_H

#include "adventos-libc.h"

typedef signed char           int8_t;
typedef short                 int16_t;
typedef int                   int32_t;
/* int64_t / uint64_t already declared by adventos-libc.h */

typedef unsigned char         uint8_t;
typedef unsigned short        uint16_t;
typedef unsigned int          uint32_t;

#endif
