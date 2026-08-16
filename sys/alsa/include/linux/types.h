// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _LINUX_TYPES_H_
#define _LINUX_TYPES_H_

#include <sys/types.h>

typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;

/* Aliases for common Linux kernel types */
typedef __u64 u64;
typedef __u32 u32;
typedef __u16 u16;
typedef __u8 u8;

/* Big-endian types (FreeBSD byte order utilities) */
typedef uint32_t __be32;
typedef uint16_t __be16;

#endif
