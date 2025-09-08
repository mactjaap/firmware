// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 1999-2024 Charles Cazabon

/*
 * This file contains some macro definitions for handling 32/64 bit platforms.
 */

#include <limits.h>

#define rand32() ((unsigned int) rand() | ( (unsigned int) rand() << 16))

#if (ULONG_MAX == 4294967295UL)
    #define FMT_TARGET "0x%08lx"
    #define rand_ul() rand32()
    #define UL_ONEBITS 0xffffffff
    #define UL_LEN 32
    #define CHECKERBOARD1 0x55555555
    #define CHECKERBOARD2 0xaaaaaaaa
    #define UL_BYTE(x) ((x | x << 8 | x << 16 | x << 24))
#elif (ULONG_MAX == 18446744073709551615ULL)
    #define FMT_TARGET "0x%016lx"
    #define rand64() (((ul) rand32()) << 32 | ((ul) rand32()))
    #define rand_ul() rand64()
    #define UL_ONEBITS 0xffffffffffffffffUL
    #define UL_LEN 64
    #define CHECKERBOARD1 0x5555555555555555
    #define CHECKERBOARD2 0xaaaaaaaaaaaaaaaa
    #define UL_BYTE(x) (((ul)x | (ul)x<<8 | (ul)x<<16 | (ul)x<<24 | (ul)x<<32 | (ul)x<<40 | (ul)x<<48 | (ul)x<<56))
#else
    #error long on this platform is not 32 or 64 bits
#endif


