#pragma once

// The part of Linux's DRM and i915 user API (include/uapi/drm/drm.h and
// i915_drm.h) that /dev/dri/renderD128 implements so far: the same ioctl
// numbers and structures, so libdrm and Mesa can use it unchanged later.
// (Struct layouts checked against Mesa 24.3's include/drm-uapi copy.)
// outside/iUserApps/gpu-test/src/i915_drm.hpp is a copy for the test.
//
// Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
// Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
// Copyright 2003 Tungsten Graphics, Inc., Cedar Park, Texas.
// All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sub license, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice (including the
// next paragraph) shall be included in all copies or substantial portions
// of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <stdint.h>

namespace drm {

// Linux's _IOC encoding: dir (2 bits: 1 write, 2 read) << 30, size << 16,
// type << 8, nr. DRM's type is 'd'; driver ioctls start at 0x40.
constexpr uint32_t kIocWrite = 1;
constexpr uint32_t kIocRead = 2;
constexpr uint32_t kIoctlType = 'd';
constexpr uint32_t kCommandBase = 0x40;

constexpr uint64_t ioc(uint32_t dir, uint32_t nr, uint32_t size) {
    return (static_cast<uint64_t>(dir) << 30) | (static_cast<uint64_t>(size) << 16) | (kIoctlType << 8) | nr;
}
// Another ioctl type: sync_file's '>' and dma-buf's 'b'.
constexpr uint64_t iocTyped(uint32_t dir, uint32_t type, uint32_t nr, uint32_t size) {
    return (static_cast<uint64_t>(dir) << 30) | (static_cast<uint64_t>(size) << 16) | (type << 8) | nr;
}
constexpr uint32_t iocNr(uint64_t request) { return request & 0xFF; }
constexpr uint32_t iocType(uint64_t request) { return (request >> 8) & 0xFF; }
constexpr uint32_t iocSize(uint64_t request) { return (request >> 16) & 0x3FFF; }
constexpr uint32_t iocDir(uint64_t request) { return (request >> 30) & 0x3; }

// Core DRM.
constexpr uint32_t DRM_VERSION_NR = 0x00;
constexpr uint32_t DRM_GEM_CLOSE_NR = 0x09;
constexpr uint32_t DRM_GET_CAP_NR = 0x0c;
constexpr uint32_t DRM_SYNCOBJ_CREATE_NR = 0xBF;
constexpr uint32_t DRM_SYNCOBJ_DESTROY_NR = 0xC0;
constexpr uint32_t DRM_SYNCOBJ_WAIT_NR = 0xC3;
constexpr uint32_t DRM_SYNCOBJ_RESET_NR = 0xC4;
constexpr uint32_t DRM_SYNCOBJ_SIGNAL_NR = 0xC5;
constexpr uint32_t DRM_PRIME_HANDLE_TO_FD_NR = 0x2d;
constexpr uint32_t DRM_PRIME_FD_TO_HANDLE_NR = 0x2e;
constexpr uint32_t DRM_SYNCOBJ_HANDLE_TO_FD_NR = 0xC1;
constexpr uint32_t DRM_SYNCOBJ_FD_TO_HANDLE_NR = 0xC2;
constexpr uint32_t DRM_SYNCOBJ_TIMELINE_WAIT_NR = 0xCA;
constexpr uint32_t DRM_SYNCOBJ_QUERY_NR = 0xCB;
constexpr uint32_t DRM_SYNCOBJ_TRANSFER_NR = 0xCC;
constexpr uint32_t DRM_SYNCOBJ_TIMELINE_SIGNAL_NR = 0xCD;

struct drm_version {
    int32_t version_major;
    int32_t version_minor;
    int32_t version_patchlevel;
    uint64_t name_len;      // size_t
    uint64_t name;          // char*
    uint64_t date_len;
    uint64_t date;
    uint64_t desc_len;
    uint64_t desc;
};

struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

constexpr uint64_t DRM_CAP_PRIME = 0x5;
constexpr uint64_t DRM_CAP_TIMESTAMP_MONOTONIC = 0x6;
constexpr uint64_t DRM_CAP_SYNCOBJ = 0x13;
constexpr uint64_t DRM_CAP_SYNCOBJ_TIMELINE = 0x14;

// Sync objects (binary so far): a handle that holds a fence or nothing.
struct drm_syncobj_create {
    uint32_t handle;        // out
    uint32_t flags;
};

constexpr uint32_t DRM_SYNCOBJ_CREATE_SIGNALED = 1u << 0;

struct drm_syncobj_destroy {
    uint32_t handle;
    uint32_t pad;
};

struct drm_syncobj_wait {
    uint64_t handles;        // uint32_t[count_handles]
    int64_t timeout_nsec;    // absolute, CLOCK_MONOTONIC
    uint32_t count_handles;
    uint32_t flags;
    uint32_t first_signaled; // out, when not waiting for all
    uint32_t pad;
    uint64_t deadline_nsec;
};

constexpr uint32_t DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL = 1u << 0;
constexpr uint32_t DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT = 1u << 1;
constexpr uint32_t DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE = 1u << 2;
constexpr uint32_t DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE = 1u << 3;

struct drm_syncobj_array {
    uint64_t handles;        // uint32_t[count_handles]
    uint32_t count_handles;
    uint32_t pad;
};

// A buffer object as a dma-buf fd (PRIME), and back.
struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;          // DRM_CLOEXEC, DRM_RDWR
    int32_t fd;
};

constexpr uint32_t DRM_CLOEXEC = 02000000;   // O_CLOEXEC
constexpr uint32_t DRM_RDWR = 2;             // O_RDWR
constexpr uint64_t DRM_PRIME_CAP_IMPORT = 1;
constexpr uint64_t DRM_PRIME_CAP_EXPORT = 2;

// A sync object as an fd (opaque, or a sync_file of its fence), and back.
struct drm_syncobj_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
    uint32_t pad;
    uint64_t point;          // with the TIMELINE flags
};

constexpr uint32_t DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE = 1u << 0;
constexpr uint32_t DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE = 1u << 1;
constexpr uint32_t DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE = 1u << 0;
constexpr uint32_t DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE = 1u << 1;

// Timeline sync objects: a point of one into a point (or the payload) of another.
struct drm_syncobj_transfer {
    uint32_t src_handle;
    uint32_t dst_handle;
    uint64_t src_point;
    uint64_t dst_point;
    uint32_t flags;
    uint32_t pad;
};

struct drm_syncobj_timeline_wait {
    uint64_t handles;        // uint32_t[count_handles]
    uint64_t points;         // uint64_t[count_handles]
    int64_t timeout_nsec;    // absolute, CLOCK_MONOTONIC
    uint32_t count_handles;
    uint32_t flags;
    uint32_t first_signaled; // out
    uint32_t pad;
    uint64_t deadline_nsec;
};

// TIMELINE_SIGNAL and QUERY.
struct drm_syncobj_timeline_array {
    uint64_t handles;
    uint64_t points;
    uint32_t count_handles;
    uint32_t flags;
};

constexpr uint32_t DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED = 1u << 0;

// sync_file (linux/sync_file.h, ioctl type '>'): an fd holding one fence.
constexpr uint32_t kSyncIoctlType = '>';
constexpr uint32_t SYNC_IOC_MERGE_NR = 3;
constexpr uint32_t SYNC_IOC_FILE_INFO_NR = 4;

struct sync_merge_data {
    char name[32];
    int32_t fd2;
    int32_t fence;           // out: the merged sync_file
    uint32_t flags;
    uint32_t pad;
};

struct sync_fence_info {
    char obj_name[32];
    char driver_name[32];
    int32_t status;          // 1 signaled, 0 active, < 0 error
    uint32_t flags;
    uint64_t timestamp_ns;
};

struct sync_file_info {
    char name[32];
    int32_t status;
    uint32_t flags;
    uint32_t num_fences;     // in: room at sync_fence_info (0: just count); out: how many
    uint32_t pad;
    uint64_t sync_fence_info;
};

// dma-buf (linux/dma-buf.h, ioctl type 'b').
constexpr uint32_t kDmaBufIoctlType = 'b';
constexpr uint32_t DMA_BUF_IOCTL_SYNC_NR = 0;
constexpr uint32_t DMA_BUF_SET_NAME_NR = 1;
constexpr uint32_t DMA_BUF_IOCTL_EXPORT_SYNC_FILE_NR = 2;
constexpr uint32_t DMA_BUF_IOCTL_IMPORT_SYNC_FILE_NR = 3;

struct dma_buf_sync {
    uint64_t flags;
};

constexpr uint64_t DMA_BUF_SYNC_READ = 1 << 0;
constexpr uint64_t DMA_BUF_SYNC_WRITE = 2 << 0;
constexpr uint64_t DMA_BUF_SYNC_RW = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE;
constexpr uint64_t DMA_BUF_SYNC_START = 0 << 2;
constexpr uint64_t DMA_BUF_SYNC_END = 1 << 2;

struct dma_buf_export_sync_file {
    uint32_t flags;          // DMA_BUF_SYNC_READ / WRITE
    int32_t fd;              // out
};

struct dma_buf_import_sync_file {
    uint32_t flags;
    int32_t fd;
};

// i915 (numbers relative to kCommandBase).
constexpr uint32_t DRM_I915_GETPARAM = 0x06;
constexpr uint32_t DRM_I915_GEM_BUSY = 0x17;
constexpr uint32_t DRM_I915_GEM_CREATE = 0x1b;
constexpr uint32_t DRM_I915_GEM_SET_DOMAIN = 0x1f;
constexpr uint32_t DRM_I915_GEM_SW_FINISH = 0x20;
constexpr uint32_t DRM_I915_GEM_MMAP_GTT = 0x24;     // DRM_IOCTL_I915_GEM_MMAP_OFFSET shares it
constexpr uint32_t DRM_I915_GEM_EXECBUFFER2 = 0x29;
constexpr uint32_t DRM_I915_GEM_WAIT = 0x2c;
constexpr uint32_t DRM_I915_GEM_CONTEXT_CREATE = 0x2d;   // also CONTEXT_CREATE_EXT (a larger struct)
constexpr uint32_t DRM_I915_GEM_CONTEXT_DESTROY = 0x2e;
constexpr uint32_t DRM_I915_GET_RESET_STATS = 0x32;
constexpr uint32_t DRM_I915_GEM_SET_TILING = 0x21;
constexpr uint32_t DRM_I915_GEM_GET_TILING = 0x22;
constexpr uint32_t DRM_I915_GEM_GET_APERTURE = 0x23;
constexpr uint32_t DRM_I915_GEM_MADVISE = 0x26;
constexpr uint32_t DRM_I915_GEM_SET_CACHING = 0x2f;
constexpr uint32_t DRM_I915_GEM_GET_CACHING = 0x30;
constexpr uint32_t DRM_I915_REG_READ = 0x31;
constexpr uint32_t DRM_I915_GEM_CONTEXT_GETPARAM = 0x34;
constexpr uint32_t DRM_I915_GEM_CONTEXT_SETPARAM = 0x35;
constexpr uint32_t DRM_I915_QUERY = 0x39;
constexpr uint32_t DRM_I915_GEM_VM_CREATE = 0x3a;
constexpr uint32_t DRM_I915_GEM_VM_DESTROY = 0x3b;
constexpr uint32_t DRM_I915_GEM_CREATE_EXT = 0x3c;

struct drm_i915_getparam {
    int32_t param;
    uint32_t pad;           // the pointer's alignment
    uint64_t value;         // int*
};

constexpr int32_t I915_PARAM_CHIPSET_ID = 4;
constexpr int32_t I915_PARAM_HAS_EXECBUF2 = 9;
constexpr int32_t I915_PARAM_HAS_BLT = 11;
constexpr int32_t I915_PARAM_HAS_LLC = 17;
constexpr int32_t I915_PARAM_HAS_WAIT_TIMEOUT = 19;
constexpr int32_t I915_PARAM_HAS_EXEC_NO_RELOC = 25;
constexpr int32_t I915_PARAM_HAS_EXEC_HANDLE_LUT = 26;
constexpr int32_t I915_PARAM_MMAP_VERSION = 30;
constexpr int32_t I915_PARAM_REVISION = 32;
constexpr int32_t I915_PARAM_HAS_EXEC_SOFTPIN = 37;
constexpr int32_t I915_PARAM_MMAP_GTT_VERSION = 40;
constexpr int32_t I915_PARAM_HAS_EXEC_ASYNC = 43;
constexpr int32_t I915_PARAM_HAS_EXEC_FENCE = 44;
constexpr int32_t I915_PARAM_HAS_EXEC_FENCE_ARRAY = 49;
constexpr int32_t I915_PARAM_HAS_EXEC_BATCH_FIRST = 48;
constexpr int32_t I915_PARAM_HAS_GEM = 5;
constexpr int32_t I915_PARAM_HAS_BSD = 10;
constexpr int32_t I915_PARAM_HAS_RELAXED_FENCING = 12;
constexpr int32_t I915_PARAM_HAS_COHERENT_RINGS = 13;
constexpr int32_t I915_PARAM_HAS_EXEC_CONSTANTS = 14;
constexpr int32_t I915_PARAM_HAS_RELAXED_DELTA = 15;
constexpr int32_t I915_PARAM_HAS_GEN7_SOL_RESET = 16;
constexpr int32_t I915_PARAM_HAS_ALIASING_PPGTT = 18;   // 3: a full 48-bit PPGTT per address space
constexpr int32_t I915_PARAM_HAS_SEMAPHORES = 20;
constexpr int32_t I915_PARAM_HAS_PRIME_VMAP_FLUSH = 21;
constexpr int32_t I915_PARAM_HAS_VEBOX = 22;
constexpr int32_t I915_PARAM_HAS_SECURE_BATCHES = 23;
constexpr int32_t I915_PARAM_HAS_PINNED_BATCHES = 24;
constexpr int32_t I915_PARAM_HAS_WT = 27;
constexpr int32_t I915_PARAM_CMD_PARSER_VERSION = 28;
constexpr int32_t I915_PARAM_HAS_COHERENT_PHYS_GTT = 29;
constexpr int32_t I915_PARAM_HAS_BSD2 = 31;
constexpr int32_t I915_PARAM_SUBSLICE_TOTAL = 33;
constexpr int32_t I915_PARAM_EU_TOTAL = 34;
constexpr int32_t I915_PARAM_HAS_GPU_RESET = 35;
constexpr int32_t I915_PARAM_HAS_RESOURCE_STREAMER = 36;
constexpr int32_t I915_PARAM_HAS_POOLED_EU = 38;
constexpr int32_t I915_PARAM_MIN_EU_IN_POOL = 39;
constexpr int32_t I915_PARAM_HAS_SCHEDULER = 41;
constexpr int32_t I915_PARAM_HUC_STATUS = 42;
constexpr int32_t I915_PARAM_HAS_EXEC_CAPTURE = 45;
constexpr int32_t I915_PARAM_SLICE_MASK = 46;
constexpr int32_t I915_PARAM_SUBSLICE_MASK = 47;
constexpr int32_t I915_PARAM_HAS_CONTEXT_ISOLATION = 50;
constexpr int32_t I915_PARAM_CS_TIMESTAMP_FREQUENCY = 51;
constexpr int32_t I915_PARAM_MMAP_GTT_COHERENT = 52;
constexpr int32_t I915_PARAM_HAS_EXEC_SUBMIT_FENCE = 53;
constexpr int32_t I915_PARAM_PERF_REVISION = 54;
constexpr int32_t I915_PARAM_HAS_EXEC_TIMELINE_FENCES = 55;
constexpr int32_t I915_PARAM_HAS_USERPTR_PROBE = 56;
constexpr int32_t I915_PARAM_OA_TIMESTAMP_FREQUENCY = 57;
constexpr int32_t I915_PARAM_PXP_STATUS = 58;
constexpr int32_t I915_PARAM_HAS_CONTEXT_FREQ_HINT = 59;

struct drm_i915_gem_create {
    uint64_t size;          // in: bytes; out: rounded up to whole pages
    uint32_t handle;        // out
    uint32_t pad;
};

struct drm_i915_gem_mmap_offset {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;        // out: pass to mmap() on the same fd
    uint64_t flags;         // I915_MMAP_OFFSET_*
    uint64_t extensions;
};

constexpr uint64_t I915_MMAP_OFFSET_GTT = 0;
constexpr uint64_t I915_MMAP_OFFSET_WC = 1;
constexpr uint64_t I915_MMAP_OFFSET_WB = 2;
constexpr uint64_t I915_MMAP_OFFSET_UC = 3;
constexpr uint64_t I915_MMAP_OFFSET_FIXED = 4;

struct drm_i915_gem_set_domain {
    uint32_t handle;
    uint32_t read_domains;
    uint32_t write_domain;
};

struct drm_i915_gem_sw_finish {
    uint32_t handle;
};

struct drm_i915_gem_busy {
    uint32_t handle;
    uint32_t busy;          // out
};

struct drm_i915_gem_wait {
    uint32_t bo_handle;
    uint32_t flags;
    int64_t timeout_ns;     // in/out
};

struct drm_i915_gem_exec_object2 {
    uint32_t handle;
    uint32_t relocation_count;
    uint64_t relocs_ptr;
    uint64_t alignment;
    uint64_t offset;        // with EXEC_OBJECT_PINNED: where it goes (canonical form)
    uint64_t flags;
    uint64_t rsvd1;
    uint64_t rsvd2;
};

constexpr uint64_t EXEC_OBJECT_NEEDS_FENCE = 1 << 0;
constexpr uint64_t EXEC_OBJECT_NEEDS_GTT = 1 << 1;
constexpr uint64_t EXEC_OBJECT_WRITE = 1 << 2;
constexpr uint64_t EXEC_OBJECT_SUPPORTS_48B_ADDRESS = 1 << 3;
constexpr uint64_t EXEC_OBJECT_PINNED = 1 << 4;
constexpr uint64_t EXEC_OBJECT_PAD_TO_SIZE = 1 << 5;
constexpr uint64_t EXEC_OBJECT_ASYNC = 1 << 6;
constexpr uint64_t EXEC_OBJECT_CAPTURE = 1 << 7;

struct drm_i915_gem_execbuffer2 {
    uint64_t buffers_ptr;   // drm_i915_gem_exec_object2[buffer_count]
    uint32_t buffer_count;
    uint32_t batch_start_offset;
    uint32_t batch_len;
    uint32_t DR1;
    uint32_t DR4;
    uint32_t num_cliprects;
    uint64_t cliprects_ptr;
    uint64_t flags;
    uint64_t rsvd1;         // context id
    uint64_t rsvd2;
};

struct drm_i915_gem_context_create_ext {
    uint32_t ctx_id;        // out
    uint32_t flags;         // I915_CONTEXT_CREATE_FLAGS_* (the plain CREATE's pad)
    uint64_t extensions;    // CREATE_EXT only
};

constexpr uint32_t I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS = 1u << 0;
constexpr uint32_t I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE = 1u << 1;

struct drm_i915_gem_context_destroy {
    uint32_t ctx_id;
    uint32_t pad;
};

constexpr uint64_t I915_EXEC_RING_MASK = 0x3f;
constexpr uint64_t I915_EXEC_DEFAULT = 0;
constexpr uint64_t I915_EXEC_RENDER = 1;
constexpr uint64_t I915_EXEC_BSD = 2;
constexpr uint64_t I915_EXEC_BLT = 3;
constexpr uint64_t I915_EXEC_VEBOX = 4;
constexpr uint64_t I915_EXEC_NO_RELOC = 1 << 11;
constexpr uint64_t I915_EXEC_HANDLE_LUT = 1 << 12;
constexpr uint64_t I915_EXEC_BATCH_FIRST = 1 << 18;
constexpr uint64_t I915_EXEC_FENCE_ARRAY = 1 << 19;   // cliprects_ptr/num_cliprects: drm_i915_gem_exec_fence[]
constexpr uint64_t I915_EXEC_FENCE_IN = 1 << 16;      // rsvd2's low half: a sync_file to wait for
constexpr uint64_t I915_EXEC_FENCE_OUT = 1 << 17;     // rsvd2's high half: a sync_file of this batch (EXECBUFFER2_WR)
constexpr uint64_t I915_EXEC_FENCE_SUBMIT = 1 << 20;
constexpr uint64_t I915_EXEC_USE_EXTENSIONS = 1 << 21;   // cliprects_ptr: an i915_user_extension chain
constexpr uint32_t DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES = 0;

struct drm_i915_gem_exec_fence {
    uint32_t handle;        // a sync object
    uint32_t flags;
};

constexpr uint32_t I915_EXEC_FENCE_WAIT = 1u << 0;
constexpr uint32_t I915_EXEC_FENCE_SIGNAL = 1u << 1;

// Extension chains (struct i915_user_extension): each block starts with the
// next one's address (0 ends the chain) and its name.
struct i915_user_extension {
    uint64_t next_extension;
    uint32_t name;
    uint32_t flags;          // must be 0
    uint32_t rsvd[4];        // must be 0
};

// CONTEXT_GETPARAM / CONTEXT_SETPARAM, and the CONTEXT_CREATE_EXT SETPARAM extension.
struct drm_i915_gem_context_param {
    uint32_t ctx_id;
    uint32_t size;           // 0 for the scalar parameters; bytes at `value` for the others
    uint64_t param;
    uint64_t value;
};

constexpr uint64_t I915_CONTEXT_PARAM_BAN_PERIOD = 0x1;
constexpr uint64_t I915_CONTEXT_PARAM_NO_ZEROMAP = 0x2;
constexpr uint64_t I915_CONTEXT_PARAM_GTT_SIZE = 0x3;
constexpr uint64_t I915_CONTEXT_PARAM_NO_ERROR_CAPTURE = 0x4;
constexpr uint64_t I915_CONTEXT_PARAM_BANNABLE = 0x5;
constexpr uint64_t I915_CONTEXT_PARAM_PRIORITY = 0x6;
constexpr uint64_t I915_CONTEXT_PARAM_SSEU = 0x7;
constexpr uint64_t I915_CONTEXT_PARAM_RECOVERABLE = 0x8;
constexpr uint64_t I915_CONTEXT_PARAM_VM = 0x9;
constexpr uint64_t I915_CONTEXT_PARAM_ENGINES = 0xa;
constexpr uint64_t I915_CONTEXT_PARAM_PERSISTENCE = 0xb;
constexpr uint64_t I915_CONTEXT_PARAM_RINGSIZE = 0xc;
constexpr uint64_t I915_CONTEXT_PARAM_PROTECTED_CONTENT = 0xd;
constexpr uint64_t I915_CONTEXT_PARAM_LOW_LATENCY = 0xe;
constexpr uint64_t I915_CONTEXT_PARAM_CONTEXT_IMAGE = 0xf;
constexpr int64_t I915_CONTEXT_MAX_USER_PRIORITY = 1023;
constexpr int64_t I915_CONTEXT_MIN_USER_PRIORITY = -1023;

constexpr uint32_t I915_CONTEXT_CREATE_EXT_SETPARAM = 0;
constexpr uint32_t I915_CONTEXT_CREATE_EXT_CLONE = 1;   // removed from Linux; EINVAL

struct drm_i915_gem_context_create_ext_setparam {
    i915_user_extension base;
    drm_i915_gem_context_param param;
};

// An engine by class and instance (the engine map, the engine query).
struct i915_engine_class_instance {
    uint16_t engine_class;
    uint16_t engine_instance;
};

constexpr uint16_t I915_ENGINE_CLASS_RENDER = 0;
constexpr uint16_t I915_ENGINE_CLASS_COPY = 1;
constexpr uint16_t I915_ENGINE_CLASS_VIDEO = 2;
constexpr uint16_t I915_ENGINE_CLASS_VIDEO_ENHANCE = 3;
constexpr uint16_t I915_ENGINE_CLASS_COMPUTE = 4;
constexpr uint16_t I915_ENGINE_CLASS_INVALID = 0xffff;
constexpr uint16_t I915_ENGINE_CLASS_INVALID_NONE = 0xffff;   // -1: an empty slot in an engine map

// I915_CONTEXT_PARAM_ENGINES: a header, then the engines (packed, 4 bytes each).
struct i915_context_param_engines {
    uint64_t extensions;     // load balancing / bonding / parallel: not supported
};

// GEM_VM_CREATE / GEM_VM_DESTROY: another address space of the file.
struct drm_i915_gem_vm_control {
    uint64_t extensions;
    uint32_t flags;
    uint32_t vm_id;          // out of VM_CREATE (never 0), in to VM_DESTROY
};

// QUERY: an array of items, each answered in place.
struct drm_i915_query_item {
    uint64_t query_id;
    int32_t length;          // 0: report the size needed; afterwards the size written or -errno
    uint32_t flags;
    uint64_t data_ptr;
};

struct drm_i915_query {
    uint32_t num_items;
    uint32_t flags;          // must be 0
    uint64_t items_ptr;
};

constexpr uint64_t DRM_I915_QUERY_TOPOLOGY_INFO = 1;
constexpr uint64_t DRM_I915_QUERY_ENGINE_INFO = 2;
constexpr uint64_t DRM_I915_QUERY_PERF_CONFIG = 3;
constexpr uint64_t DRM_I915_QUERY_MEMORY_REGIONS = 4;
constexpr uint64_t DRM_I915_QUERY_HWCONFIG_BLOB = 5;
constexpr uint64_t DRM_I915_QUERY_GEOMETRY_SUBSLICES = 6;
constexpr uint64_t DRM_I915_QUERY_GUC_SUBMISSION_VERSION = 7;

// Followed by data[]: the slice mask, then subslice masks per slice from
// subslice_offset, then EU masks per subslice from eu_offset.
struct drm_i915_query_topology_info {
    uint16_t flags;
    uint16_t max_slices;
    uint16_t max_subslices;
    uint16_t max_eus_per_subslice;
    uint16_t subslice_offset;
    uint16_t subslice_stride;
    uint16_t eu_offset;
    uint16_t eu_stride;
};

struct drm_i915_engine_info {
    i915_engine_class_instance engine;
    uint32_t rsvd0;
    uint64_t flags;
    uint64_t capabilities;
    uint16_t logical_instance;
    uint16_t rsvd1[3];
    uint64_t rsvd2[3];
};

constexpr uint64_t I915_ENGINE_INFO_HAS_LOGICAL_INSTANCE = 1 << 0;

struct drm_i915_query_engine_info {
    uint32_t num_engines;
    uint32_t rsvd[3];
    // drm_i915_engine_info engines[];
};

struct drm_i915_gem_memory_class_instance {
    uint16_t memory_class;
    uint16_t memory_instance;
};

constexpr uint16_t I915_MEMORY_CLASS_SYSTEM = 0;
constexpr uint16_t I915_MEMORY_CLASS_DEVICE = 1;

struct drm_i915_memory_region_info {
    drm_i915_gem_memory_class_instance region;
    uint32_t rsvd0;
    uint64_t probed_size;
    uint64_t unallocated_size;
    uint64_t probed_cpu_visible_size;      // the union's struct, then rsvd1[2..7]
    uint64_t unallocated_cpu_visible_size;
    uint64_t rsvd1[6];
};

struct drm_i915_query_memory_regions {
    uint32_t num_regions;
    uint32_t rsvd[3];
    // drm_i915_memory_region_info regions[];
};

// REG_READ: only the render engine's timestamp (RING_TIMESTAMP), as i915.
struct drm_i915_reg_read {
    uint64_t offset;         // the register, its low bits flags
    uint64_t val;
};

constexpr uint64_t I915_REG_READ_8B_WA = 1 << 0;   // read it as two dwords

struct drm_i915_gem_get_aperture {
    uint64_t aper_size;
    uint64_t aper_available_size;
};

struct drm_i915_gem_set_tiling {
    uint32_t handle;
    uint32_t tiling_mode;
    uint32_t stride;
    uint32_t swizzle_mode;   // out
};

struct drm_i915_gem_get_tiling {
    uint32_t handle;
    uint32_t tiling_mode;
    uint32_t swizzle_mode;
    uint32_t phys_swizzle_mode;
};

constexpr uint32_t I915_TILING_NONE = 0;
constexpr uint32_t I915_TILING_X = 1;
constexpr uint32_t I915_TILING_Y = 2;
constexpr uint32_t I915_BIT_6_SWIZZLE_NONE = 0;

struct drm_i915_gem_caching {
    uint32_t handle;
    uint32_t caching;
};

constexpr uint32_t I915_CACHING_NONE = 0;
constexpr uint32_t I915_CACHING_CACHED = 1;
constexpr uint32_t I915_CACHING_DISPLAY = 2;

struct drm_i915_gem_madvise {
    uint32_t handle;
    uint32_t madv;
    uint32_t retained;       // out: the pages are still there
};

constexpr uint32_t I915_MADV_WILLNEED = 0;
constexpr uint32_t I915_MADV_DONTNEED = 1;

struct drm_i915_gem_create_ext {
    uint64_t size;
    uint32_t handle;
    uint32_t flags;
    uint64_t extensions;
};

constexpr uint32_t I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS = 1u << 0;
constexpr uint32_t I915_GEM_CREATE_EXT_MEMORY_REGIONS = 0;
constexpr uint32_t I915_GEM_CREATE_EXT_PROTECTED_CONTENT = 1;
constexpr uint32_t I915_GEM_CREATE_EXT_SET_PAT = 2;

struct drm_i915_gem_create_ext_memory_regions {
    i915_user_extension base;
    uint32_t pad;
    uint32_t num_regions;
    uint64_t regions;        // drm_i915_gem_memory_class_instance[num_regions]
};

// Execbuffer's timeline fences: the fence array with a point for each.
struct drm_i915_gem_execbuffer_ext_timeline_fences {
    i915_user_extension base;
    uint64_t fence_count;
    uint64_t handles_ptr;    // drm_i915_gem_exec_fence[fence_count]
    uint64_t values_ptr;     // uint64_t[fence_count]: 0 for a binary sync object
};

struct drm_i915_reset_stats {
    uint32_t ctx_id;
    uint32_t flags;
    uint32_t reset_count;   // all resets (privileged callers only)
    uint32_t batch_active;  // this context's batches that were running when the GPU hung
    uint32_t batch_pending; // this context's batches lost though they were not running
    uint32_t pad;
};

static_assert(sizeof(drm_version) == 64);
static_assert(sizeof(drm_i915_getparam) == 16);
static_assert(sizeof(drm_i915_gem_create) == 16);
static_assert(sizeof(drm_i915_gem_mmap_offset) == 32);
static_assert(sizeof(drm_i915_gem_wait) == 16);
static_assert(sizeof(drm_i915_gem_exec_object2) == 56);
static_assert(sizeof(drm_i915_gem_execbuffer2) == 64);
static_assert(sizeof(drm_i915_gem_context_create_ext) == 16);
static_assert(sizeof(drm_syncobj_wait) == 40);
static_assert(sizeof(drm_i915_reset_stats) == 24);
static_assert(sizeof(i915_user_extension) == 32);
static_assert(sizeof(drm_i915_gem_context_param) == 24);
static_assert(sizeof(drm_i915_gem_context_create_ext_setparam) == 56);
static_assert(sizeof(drm_i915_gem_vm_control) == 16);
static_assert(sizeof(drm_i915_query_item) == 24);
static_assert(sizeof(drm_i915_query) == 16);
static_assert(sizeof(drm_i915_query_topology_info) == 16);
static_assert(sizeof(drm_i915_engine_info) == 56);
static_assert(sizeof(drm_i915_query_engine_info) == 16);
static_assert(sizeof(drm_i915_memory_region_info) == 88);
static_assert(sizeof(drm_i915_query_memory_regions) == 16);
static_assert(sizeof(drm_i915_gem_create_ext) == 24);
static_assert(sizeof(drm_i915_gem_create_ext_memory_regions) == 48);
static_assert(sizeof(drm_i915_gem_madvise) == 12);
static_assert(sizeof(drm_prime_handle) == 12);
static_assert(sizeof(drm_syncobj_handle) == 24);
static_assert(sizeof(drm_syncobj_transfer) == 32);
static_assert(sizeof(drm_syncobj_timeline_wait) == 48);
static_assert(sizeof(drm_syncobj_timeline_array) == 24);
static_assert(sizeof(sync_merge_data) == 48);
static_assert(sizeof(sync_fence_info) == 80);
static_assert(sizeof(sync_file_info) == 56);
static_assert(sizeof(drm_i915_gem_execbuffer_ext_timeline_fences) == 56);

constexpr uint64_t DRM_IOCTL_VERSION = ioc(kIocRead | kIocWrite, DRM_VERSION_NR, sizeof(drm_version));
constexpr uint64_t DRM_IOCTL_GEM_CLOSE = ioc(kIocWrite, DRM_GEM_CLOSE_NR, sizeof(drm_gem_close));
constexpr uint64_t DRM_IOCTL_I915_GETPARAM =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GETPARAM, sizeof(drm_i915_getparam));
constexpr uint64_t DRM_IOCTL_I915_GEM_CREATE =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_CREATE, sizeof(drm_i915_gem_create));
constexpr uint64_t DRM_IOCTL_I915_GEM_MMAP_OFFSET =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_MMAP_GTT, sizeof(drm_i915_gem_mmap_offset));
constexpr uint64_t DRM_IOCTL_I915_GEM_SET_DOMAIN =
    ioc(kIocWrite, kCommandBase + DRM_I915_GEM_SET_DOMAIN, sizeof(drm_i915_gem_set_domain));
constexpr uint64_t DRM_IOCTL_I915_GEM_SW_FINISH =
    ioc(kIocWrite, kCommandBase + DRM_I915_GEM_SW_FINISH, sizeof(drm_i915_gem_sw_finish));
constexpr uint64_t DRM_IOCTL_I915_GEM_BUSY =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_BUSY, sizeof(drm_i915_gem_busy));
constexpr uint64_t DRM_IOCTL_I915_GEM_EXECBUFFER2 =
    ioc(kIocWrite, kCommandBase + DRM_I915_GEM_EXECBUFFER2, sizeof(drm_i915_gem_execbuffer2));
constexpr uint64_t DRM_IOCTL_GET_CAP = ioc(kIocRead | kIocWrite, DRM_GET_CAP_NR, sizeof(drm_get_cap));
constexpr uint64_t DRM_IOCTL_SYNCOBJ_CREATE = ioc(kIocRead | kIocWrite, DRM_SYNCOBJ_CREATE_NR, sizeof(drm_syncobj_create));
constexpr uint64_t DRM_IOCTL_SYNCOBJ_DESTROY = ioc(kIocRead | kIocWrite, DRM_SYNCOBJ_DESTROY_NR, sizeof(drm_syncobj_destroy));
constexpr uint64_t DRM_IOCTL_SYNCOBJ_WAIT = ioc(kIocRead | kIocWrite, DRM_SYNCOBJ_WAIT_NR, sizeof(drm_syncobj_wait));
constexpr uint64_t DRM_IOCTL_SYNCOBJ_RESET = ioc(kIocRead | kIocWrite, DRM_SYNCOBJ_RESET_NR, sizeof(drm_syncobj_array));
constexpr uint64_t DRM_IOCTL_SYNCOBJ_SIGNAL = ioc(kIocRead | kIocWrite, DRM_SYNCOBJ_SIGNAL_NR, sizeof(drm_syncobj_array));
constexpr uint64_t DRM_IOCTL_I915_GET_RESET_STATS =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GET_RESET_STATS, sizeof(drm_i915_reset_stats));
constexpr uint64_t DRM_IOCTL_I915_GEM_CONTEXT_CREATE =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_CONTEXT_CREATE, 8);
constexpr uint64_t DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_CONTEXT_CREATE, sizeof(drm_i915_gem_context_create_ext));
constexpr uint64_t DRM_IOCTL_I915_GEM_CONTEXT_DESTROY =
    ioc(kIocWrite, kCommandBase + DRM_I915_GEM_CONTEXT_DESTROY, sizeof(drm_i915_gem_context_destroy));
constexpr uint64_t DRM_IOCTL_I915_GEM_WAIT =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_WAIT, sizeof(drm_i915_gem_wait));
constexpr uint64_t DRM_IOCTL_I915_GEM_SET_TILING =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_SET_TILING, sizeof(drm_i915_gem_set_tiling));
constexpr uint64_t DRM_IOCTL_I915_GEM_GET_TILING =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_GET_TILING, sizeof(drm_i915_gem_get_tiling));
constexpr uint64_t DRM_IOCTL_I915_GEM_GET_APERTURE =
    ioc(kIocRead, kCommandBase + DRM_I915_GEM_GET_APERTURE, sizeof(drm_i915_gem_get_aperture));
constexpr uint64_t DRM_IOCTL_I915_GEM_MADVISE =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_MADVISE, sizeof(drm_i915_gem_madvise));
constexpr uint64_t DRM_IOCTL_I915_GEM_SET_CACHING =
    ioc(kIocWrite, kCommandBase + DRM_I915_GEM_SET_CACHING, sizeof(drm_i915_gem_caching));
constexpr uint64_t DRM_IOCTL_I915_GEM_GET_CACHING =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_GET_CACHING, sizeof(drm_i915_gem_caching));
constexpr uint64_t DRM_IOCTL_I915_REG_READ =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_REG_READ, sizeof(drm_i915_reg_read));
constexpr uint64_t DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_CONTEXT_GETPARAM, sizeof(drm_i915_gem_context_param));
constexpr uint64_t DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_CONTEXT_SETPARAM, sizeof(drm_i915_gem_context_param));
constexpr uint64_t DRM_IOCTL_I915_QUERY =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_QUERY, sizeof(drm_i915_query));
constexpr uint64_t DRM_IOCTL_I915_GEM_VM_CREATE =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_VM_CREATE, sizeof(drm_i915_gem_vm_control));
constexpr uint64_t DRM_IOCTL_I915_GEM_VM_DESTROY =
    ioc(kIocWrite, kCommandBase + DRM_I915_GEM_VM_DESTROY, sizeof(drm_i915_gem_vm_control));
constexpr uint64_t DRM_IOCTL_I915_GEM_EXECBUFFER2_WR =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_EXECBUFFER2, sizeof(drm_i915_gem_execbuffer2));
constexpr uint64_t DRM_IOCTL_PRIME_HANDLE_TO_FD = ioc(kIocRead | kIocWrite, DRM_PRIME_HANDLE_TO_FD_NR, sizeof(drm_prime_handle));
constexpr uint64_t DRM_IOCTL_PRIME_FD_TO_HANDLE = ioc(kIocRead | kIocWrite, DRM_PRIME_FD_TO_HANDLE_NR, sizeof(drm_prime_handle));
constexpr uint64_t DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD = ioc(kIocRead | kIocWrite, DRM_SYNCOBJ_HANDLE_TO_FD_NR, sizeof(drm_syncobj_handle));
constexpr uint64_t DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE = ioc(kIocRead | kIocWrite, DRM_SYNCOBJ_FD_TO_HANDLE_NR, sizeof(drm_syncobj_handle));
constexpr uint64_t DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT =
    ioc(kIocRead | kIocWrite, DRM_SYNCOBJ_TIMELINE_WAIT_NR, sizeof(drm_syncobj_timeline_wait));
constexpr uint64_t DRM_IOCTL_SYNCOBJ_QUERY = ioc(kIocRead | kIocWrite, DRM_SYNCOBJ_QUERY_NR, sizeof(drm_syncobj_timeline_array));
constexpr uint64_t DRM_IOCTL_SYNCOBJ_TRANSFER = ioc(kIocRead | kIocWrite, DRM_SYNCOBJ_TRANSFER_NR, sizeof(drm_syncobj_transfer));
constexpr uint64_t DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL =
    ioc(kIocRead | kIocWrite, DRM_SYNCOBJ_TIMELINE_SIGNAL_NR, sizeof(drm_syncobj_timeline_array));
constexpr uint64_t SYNC_IOC_MERGE = iocTyped(kIocRead | kIocWrite, kSyncIoctlType, SYNC_IOC_MERGE_NR, sizeof(sync_merge_data));
constexpr uint64_t SYNC_IOC_FILE_INFO = iocTyped(kIocRead | kIocWrite, kSyncIoctlType, SYNC_IOC_FILE_INFO_NR, sizeof(sync_file_info));
constexpr uint64_t DMA_BUF_IOCTL_SYNC = iocTyped(kIocWrite, kDmaBufIoctlType, DMA_BUF_IOCTL_SYNC_NR, sizeof(dma_buf_sync));
constexpr uint64_t DMA_BUF_IOCTL_EXPORT_SYNC_FILE =
    iocTyped(kIocRead | kIocWrite, kDmaBufIoctlType, DMA_BUF_IOCTL_EXPORT_SYNC_FILE_NR, sizeof(dma_buf_export_sync_file));
constexpr uint64_t DMA_BUF_IOCTL_IMPORT_SYNC_FILE =
    iocTyped(kIocWrite, kDmaBufIoctlType, DMA_BUF_IOCTL_IMPORT_SYNC_FILE_NR, sizeof(dma_buf_import_sync_file));
constexpr uint64_t DRM_IOCTL_I915_GEM_CREATE_EXT =
    ioc(kIocRead | kIocWrite, kCommandBase + DRM_I915_GEM_CREATE_EXT, sizeof(drm_i915_gem_create_ext));

} // namespace drm
