// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 1999-2024 Charles Cazabon

/*
 * This file contains typedefs, structure, and union definitions.
 */

#include "sizes.h"

typedef unsigned long ul;
typedef unsigned long long ull;
typedef unsigned long volatile ulv;
typedef unsigned char volatile u8v;
typedef unsigned short volatile u16v;

struct test {
    char *name;
    int (*fp)(ulv *, ulv*, size_t);
};
