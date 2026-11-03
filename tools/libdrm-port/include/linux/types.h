/* The fixed-width types of Linux's uapi linux/types.h that its DRM headers
 * (drm.h, i915_drm.h) use, for ports built against mlibc; installed into its
 * sysroot by tools/build-libdrm.sh. */
#ifndef INSTANTOS_LINUX_TYPES_H
#define INSTANTOS_LINUX_TYPES_H

#include <stdint.h>

typedef int8_t __s8;
typedef uint8_t __u8;
typedef int16_t __s16;
typedef uint16_t __u16;
typedef int32_t __s32;
typedef uint32_t __u32;
typedef int64_t __s64;
typedef uint64_t __u64;

typedef uint16_t __le16;
typedef uint16_t __be16;
typedef uint32_t __le32;
typedef uint32_t __be32;
typedef uint64_t __le64;
typedef uint64_t __be64;

typedef unsigned long __kernel_size_t;

#endif
