#pragma once

#include <stdint.h>

// /dev/dri/renderD128 for the Intel Gen9 GPU (intel_gen9_drm.cpp): Linux's
// DRM render node with the i915 ioctls (graphics/i915_drm.hpp), so far the
// GEM part, after i915_gem_create.c, i915_gem_mman.c and
// i915_gem_execbuffer.c (MIT):
//   * each open of the node is a DRM file with its own buffer handles and its
//     own 48-bit GPU address space (a PPGTT; one per file, as with i915's
//     default context);
//   * GEM_CREATE makes a buffer object of RAM pages, zeroed; GEM_CLOSE drops
//     the handle (a mapping keeps the object alive until it goes);
//   * GEM_MMAP_OFFSET (WB, WC, UC; FIXED is WB: the GPU shares the LLC) and
//     mmap() on the fd map it into the process;
//   * EXECBUFFER2 with softpinned objects only (EXEC_OBJECT_PINNED, no
//     relocations: what iris and anv do on Gen9) binds them at their
//     addresses and runs the batch on the render or blitter engine, waiting
//     until it has finished (no fences yet: GEM_WAIT and GEM_BUSY report idle);
//   * VERSION and GETPARAM for the parameters this implements.
// Not yet: contexts and VMs of one's own, fences/syncobjs, PRIME, the 3D
// state the render engine needs for Mesa (golden context, workarounds).

class VNode;
struct MappedObject;

namespace drm {

// The node opens only where the Intel driver runs, or after gpu-test's
// "drm without a GPU" switch (QEMU checks of the CPU side; execbuffer then
// fails with ENODEV).
bool available();
void allowWithoutGpu();

// A fresh VNode for one open (the caller wraps it in a FileDescriptor);
// nullptr if the node is not available or memory ran out.
VNode* createFileNode();
bool isRenderNode(VNode* node);
// Any fd the DRM code made: the render node, a dma-buf (PRIME), a sync_file,
// a sync object fd. ioctl, poll and (render node, dma-buf) mmap go to it.
bool isDrmNode(VNode* node);
// Writes the render node's sysfs entries under /sys (intel_gen9_sysfs.cpp), once.
void publishSysfs();
int16_t poll(VNode* node, int16_t events);

// The ioctl on an open node; 0/positive or -errno.
uint64_t ioctl(VNode* node, uint64_t request, uint64_t arg);

// What mmap(fd, offset, length) maps: the object's pages from the one at
// `offset`'s page on, and the page-table cache bits for its mode.
struct MmapTarget {
    MappedObject* object;
    const uint64_t* pages;   // physical, `length / 4096` of them
    uint64_t cacheFlags;     // 0 (WB), WriteCombine or CacheDisab | WriteThru
};
// A dma-buf's size in bytes; 0 if the node is not one.
uint64_t dmaBufSize(VNode* node);
// Pages the display scans out as an uncached dma-buf of the calling process
// (the Display syscall's ExportBuffer; they must outlive it). Its handle, or
// -errno.
int64_t exportScanoutPages(const uint64_t* frames, uint32_t count);
// The object behind a dma-buf node as a new dma-buf of the calling process
// (SurfaceExportBuffer): its handle, or -errno.
int64_t reexportDmaBuf(VNode* node);
// Waits (up to timeoutMs) until nothing writes the dma-buf's object, i.e. the
// GPU has finished drawing into it. True if so; true for anything else.
bool waitDmaBufWritten(VNode* node, uint64_t timeoutMs);

// 0 or -errno.
uint64_t resolveMmap(VNode* node, uint64_t offset, uint64_t length, MmapTarget* out);

} // namespace drm
