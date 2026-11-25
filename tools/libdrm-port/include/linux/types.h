/* The fixed-width types of Linux's uapi linux/types.h that its DRM headers
 * (drm.h, i915_drm.h) use, for ports built against mlibc; installed into its
 * sysroot by tools/build-libdrm.sh. Linux's definitions and guard
 * (asm-generic/int-ll64.h: the 64-bit ones are long long), so code that also
 * defines them, as e2fsprogs' ext2_types.h does unless _LINUX_TYPES_H is set,
 * agrees with these. */
#ifndef _LINUX_TYPES_H
#define _LINUX_TYPES_H
#define INSTANTOS_LINUX_TYPES_H

typedef signed char __s8;
typedef unsigned char __u8;
typedef signed short __s16;
typedef unsigned short __u16;
typedef signed int __s32;
typedef unsigned int __u32;
typedef signed long long __s64;
typedef unsigned long long __u64;

typedef __u16 __le16;
typedef __u16 __be16;
typedef __u32 __le32;
typedef __u32 __be32;
typedef __u64 __le64;
typedef __u64 __be64;

typedef unsigned long __kernel_size_t;

#endif
