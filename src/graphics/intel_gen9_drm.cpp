// /dev/dri/renderD128 (see intel_gen9_drm.hpp): the i915 GEM ioctls over the
// Gen9 driver. Follows Linux's i915 (MIT): i915_gem_create.c (objects),
// i915_gem_mman.c (mmap offsets), i915_gem_execbuffer.c (softpinned
// execbuffer, implicit and explicit fencing), i915_gem_wait.c,
// i915_gem_busy.c, i915_getparam.c, i915_query.c, i915_gem_context.c
// (parameters, address spaces), i915_gem_tiling.c, i915_gem_domain.c
// (caching), drm_syncobj.c (binary and timeline sync objects, their fds),
// drm_prime.c (dma-buf fds), sync_file.c and dma-buf.c. Everything here runs
// under the big kernel lock. EXECBUFFER2 queues a request and returns; the
// request holds references on its objects, its context and (through the
// file's count of requests) the file and its address space until it
// retires, which may happen in the GPU's interrupt.

#include <graphics/intel_gen9_drm.hpp>

#include <common/string.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/process/handles.hpp>
#include <cpu/process/process.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/syscall/syscall.hpp>
#include <fs/vfs/vfs.hpp>
#include <graphics/i915_drm.hpp>
#include <graphics/intel_gen9.hpp>
#include <memory/heap.hpp>
#include <memory/mapped_object.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <time/tsc_timer.hpp>

namespace drm {

namespace {

using intel_gen9::EngineId;
using intel_gen9::Fence;
using intel_gen9::Request;
using intel_gen9::GpuBuffer;
using intel_gen9::HwContext;
using intel_gen9::IntelGen9Driver;
using intel_gen9::Ppgtt;

constexpr uint64_t kPage = 4096;
constexpr uint64_t kMaxObjectBytes = 1ull << 30;      // 1 GiB, a sanity bound
constexpr uint32_t kMaxExecObjects = 4096;
constexpr uint64_t kVaBits = 48;
constexpr uint64_t kVaLimit = 1ull << kVaBits;
constexpr uint32_t kMaxExecFences = 4096;
constexpr uint32_t kMaxQueued = 512;     // requests queued on the GPU before execbuffer waits
constexpr uint64_t kForever = ~0ull;
constexpr uint32_t kMaxContexts = 64;   // per file, the default one (id 0) included

// Linux errno values (the syscall ABI's).
constexpr int64_t kEPERM = 1;
constexpr int64_t kENOENT = 2;
constexpr int64_t kEIO = 5;
constexpr int64_t kENOMEM = 12;
constexpr int64_t kEFAULT = 14;
constexpr int64_t kEBUSY = 16;
constexpr int64_t kENODEV = 19;
constexpr int64_t kEINVAL = 22;
constexpr int64_t kEMFILE = 24;
constexpr int64_t kENOTTY = 25;
constexpr int64_t kENOSPC = 28;
constexpr int64_t kETIME = 62;

uint64_t err(int64_t code) {
    return static_cast<uint64_t>(-code);
}

bool gAllowWithoutGpu = false;

void log(const char* text) {
    Cereal::get().write(text);
}

void logDec(uint64_t value) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char text[24];
    for (int i = 0; i < n; ++i) {
        text[i] = digits[n - 1 - i];
    }
    text[n] = '\0';
    log(text);
}

bool gpuPresent() {
    return IntelGen9Driver::get().isInitialized();
}


// A buffer object: RAM pages, zeroed. The handles, mappings and GPU bindings
// that use it each hold a reference.
class GemObject final : public MappedObject {
public:
    uint32_t refs = 1;
    uint64_t size = 0;
    uint32_t pageCount = 0;
    uint64_t* pages = nullptr;
    // Implicit fencing (i915's dma_resv): the last request that used it on
    // each engine, and the last that wrote it (and on which engine).
    Fence* reads[2] = {};
    Fence* write = nullptr;
    uint8_t writeEngine = 0;
    // What SET_TILING, SET_CACHING and MADVISE recorded. Tiling is only
    // metadata here (no aperture mmaps, no swizzling on Gen9); an uncached
    // object is mapped uncached in the PPGTT and flushed from the CPU caches
    // before the GPU uses it.
    uint32_t tiling = I915_TILING_NONE;
    uint32_t stride = 0;
    bool cached = true;          // I915_CACHING_CACHED, the default on an LLC part
    uint32_t madv = I915_MADV_WILLNEED;   // nothing is ever purged yet
    bool borrowed = false;       // the pages are someone else's (a scanout buffer): not freed with it

    void retain() override { ++refs; }
    void release() override {
        if (--refs == 0) {
            destroy();
        }
    }

    static GemObject* create(uint64_t bytes) {
        auto* object = new GemObject();
        if (!object) {
            return nullptr;
        }
        object->size = bytes;
        object->pageCount = static_cast<uint32_t>(bytes / kPage);
        object->pages = static_cast<uint64_t*>(kmalloc(sizeof(uint64_t) * object->pageCount));
        if (!object->pages) {
            delete object;
            return nullptr;
        }
        for (uint32_t i = 0; i < object->pageCount; ++i) {
            object->pages[i] = 0;
        }
        for (uint32_t i = 0; i < object->pageCount; ++i) {
            const uint64_t phys = PMM::AllocFrame();
            if (phys == 0) {
                object->destroy();
                return nullptr;
            }
            object->pages[i] = phys;
            // Zeroed, and no dirty line left behind: a write-combining or
            // uncached mapping of the page must not be overtaken by one.
            auto* bytesOf = reinterpret_cast<uint8_t*>(VMM::PhysToVirt(phys));
            memset(bytesOf, 0, kPage);
            for (uint64_t line = 0; line < kPage; line += 64) {
                asm volatile("clflush (%0)" :: "r"(bytesOf + line) : "memory");
            }
        }
        asm volatile("mfence" ::: "memory");
        return object;
    }

    // An object over pages it does not own (they must outlive it).
    static GemObject* borrow(const uint64_t* frames, uint32_t count) {
        auto* object = new GemObject();
        if (!object) {
            return nullptr;
        }
        object->pages = static_cast<uint64_t*>(kmalloc(sizeof(uint64_t) * count));
        if (!object->pages) {
            delete object;
            return nullptr;
        }
        for (uint32_t i = 0; i < count; ++i) {
            object->pages[i] = frames[i];
        }
        object->pageCount = count;
        object->size = static_cast<uint64_t>(count) * kPage;
        object->borrowed = true;
        return object;
    }

    // The pages as the PPGTT code takes them.
    GpuBuffer view() const {
        GpuBuffer buffer = {};
        buffer.size = size;
        buffer.pageCount = pageCount;
        buffer.pages = pages;
        return buffer;
    }

private:
    void destroy() {
        for (Fence* fence : reads) {
            if (fence) fence->release();
        }
        if (write) {
            write->release();
        }
        if (pages) {
            for (uint32_t i = 0; i < pageCount && !borrowed; ++i) {
                if (pages[i] != 0) {
                    PMM::FreeFrame(pages[i]);
                }
            }
            kfree(pages);
        }
        delete this;
    }
};

void setFence(Fence*& slot, Fence* fence) {
    if (fence) {
        fence->retain();
    }
    if (slot) {
        slot->release();
    }
    slot = fence;
}

bool fenceBusy(Fence* fence) {
    return fence && !fence->poll();
}

bool objectIdle(const GemObject* object) {
    return !fenceBusy(object->reads[0]) && !fenceBusy(object->reads[1]) && !fenceBusy(object->write);
}

// Where an object is bound in an address space.
struct Binding {
    uint64_t va;
    uint64_t size;
    GemObject* object;   // referenced
    Binding* next;
};

struct DrmFile;

// An address space (i915's i915_address_space and its VM id): a PPGTT, made
// at the first bind, and what is bound in it. Each file has a default one;
// GEM_VM_CREATE makes more. The file's default, its id table and every
// context using it hold a reference.
struct DrmVm {
    DrmFile* owner = nullptr;
    Ppgtt* ppgtt = nullptr;
    Binding* bindings = nullptr;
    uint32_t refs = 1;
    uint32_t id = 0;            // its VM id in the file (0: none given out)
    DrmVm* next = nullptr;      // the file's live address spaces
};

constexpr uint32_t kMaxEngineMap = 64;   // i915's limit too

// A GEM context (i915's i915_gem_context): the GPU state of one stream of
// work, a persistent hardware context per engine, made when the engine is
// first used, in an address space (the file's default unless set).
struct DrmContext {
    // The hardware contexts: with the legacy selectors 0 is the render
    // engine's and 1 the blitter's; with an engine map, one per slot, as i915
    // has an intel_context per slot. Mesa's iris maps its render and compute
    // batches to two slots of the render engine, and each sets up its own
    // pipeline (PIPELINE_SELECT, STATE_BASE_ADDRESS) once: sharing one image,
    // the compute batch's set-up replaced the render batch's, and its draws
    // after the first batch went nowhere.
    HwContext engines[kMaxEngineMap];
    DrmVm* vm = nullptr;        // referenced
    uint32_t pending = 0;       // its requests queued or running
    bool destroyed = false;     // CONTEXT_DESTROY came while some were pending
    bool used = false;          // a request was queued: VM and ENGINES are fixed from then
    // A batch of it hung: later ones fail with EIO (i915: a context that is
    // not recoverable, as Mesa makes them, is banned; it then makes a new one).
    bool banned = false;
    uint32_t guilty = 0;        // its batches that hung (GET_RESET_STATS)
    // CONTEXT_SETPARAM, with i915's defaults.
    bool recoverable = true;
    bool bannable = true;
    bool noErrorCapture = false;
    bool noZeromap = false;
    int64_t priority = 0;
    // I915_CONTEXT_PARAM_ENGINES: execbuffer's ring field indexes this map
    // (none: the legacy selectors).
    uint32_t engineMapCount = 0;
    i915_engine_class_instance engineMap[kMaxEngineMap] = {};
    // I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE: requests run in submission
    // order across engines (each waits for the last one's fence).
    bool singleTimeline = false;
    Fence* timeline = nullptr;  // referenced
};

// A timeline point: the value and the fence of the work that reaches it.
struct TimelinePoint {
    uint64_t point;
    Fence* fence;            // referenced
};

// A sync object (drm_syncobj), shared by the handles of any files and by its
// fds. As a binary one it holds a fence or none. As a timeline it holds
// points in ascending order; waiting for point p means for every point up to
// the first at or past p (Linux's dma_fence_chain), and points signaled
// without error drop off the front. `fence` is then the newest point's
// (what a binary wait, or point 0, sees).
struct Syncobj {
    uint32_t refs = 1;
    Fence* fence = nullptr;              // referenced
    TimelinePoint* points = nullptr;     // kmalloc'd
    uint32_t pointCount = 0;
    uint32_t pointCapacity = 0;
    uint64_t lastPoint = 0;              // the highest point added
    uint64_t signaledPoint = 0;          // the highest point dropped as signaled
};

void retainSync(Syncobj* sync) {
    ++sync->refs;
}

void clearPoints(Syncobj* sync) {
    for (uint32_t i = 0; i < sync->pointCount; ++i) {
        sync->points[i].fence->release();
    }
    sync->pointCount = 0;
    sync->lastPoint = 0;
    sync->signaledPoint = 0;
}

void releaseSync(Syncobj* sync) {
    if (--sync->refs != 0) {
        return;
    }
    setFence(sync->fence, nullptr);
    clearPoints(sync);
    if (sync->points) {
        kfree(sync->points);
    }
    delete sync;
}

constexpr uint32_t kMaxVms = 64;   // VM ids per file

// One open of the render node.
struct DrmFile {
    DrmContext* contexts[kMaxContexts] = {};   // by id; 0 is the default (made at open)
    Syncobj** syncobjs = nullptr;              // handle h is syncobjs[h - 1] (referenced)
    uint32_t syncobjCapacity = 0;
    GemObject** handles = nullptr;   // handle h is handles[h - 1]
    uint32_t handleCapacity = 0;
    DrmVm* defaultVm = nullptr;      // referenced
    DrmVm* vmIds[kMaxVms] = {};      // VM id v is vmIds[v - 1] (referenced)
    DrmVm* vms = nullptr;            // every live address space
    uint32_t inflight = 0;           // its requests not yet retired
    bool closed = false;
};

DrmFile* fileOf(VNode* node) {
    return node ? static_cast<DrmFile*>(node->getData()) : nullptr;
}

GemObject* lookup(DrmFile* file, uint32_t handle) {
    if (handle == 0 || handle > file->handleCapacity) {
        return nullptr;
    }
    return file->handles[handle - 1];
}

// The lowest free handle (i915 hands them out like fds), 0 if out of memory.
uint32_t addHandle(DrmFile* file, GemObject* object) {
    for (uint32_t i = 0; i < file->handleCapacity; ++i) {
        if (!file->handles[i]) {
            file->handles[i] = object;
            return i + 1;
        }
    }
    const uint32_t capacity = file->handleCapacity ? file->handleCapacity * 2 : 16;
    auto* grown = static_cast<GemObject**>(kmalloc(sizeof(GemObject*) * capacity));
    if (!grown) {
        return 0;
    }
    for (uint32_t i = 0; i < capacity; ++i) {
        grown[i] = i < file->handleCapacity ? file->handles[i] : nullptr;
    }
    const uint32_t handle = file->handleCapacity + 1;
    grown[handle - 1] = object;
    if (file->handles) {
        kfree(file->handles);
    }
    file->handles = grown;
    file->handleCapacity = capacity;
    return handle;
}

DrmVm* createVm(DrmFile* file) {
    auto* vm = new DrmVm();
    if (!vm) {
        return nullptr;
    }
    vm->owner = file;
    vm->next = file->vms;
    file->vms = vm;
    return vm;
}

DrmVm* lookupVm(DrmFile* file, uint64_t id) {
    return id == 0 || id > kMaxVms ? nullptr : file->vmIds[id - 1];
}

void unbind(DrmVm* vm, Binding* binding) {
    Binding** link = &vm->bindings;
    while (*link && *link != binding) {
        link = &(*link)->next;
    }
    if (!*link) {
        return;
    }
    *link = binding->next;
    if (vm->ppgtt) {
        IntelGen9Driver::get().ppgttUnmap(vm->ppgtt, binding->va, binding->size);
    }
    binding->object->release();
    delete binding;
}

// Every binding of `object` (or of all objects) in one address space.
void unbindAll(DrmVm* vm, GemObject* object) {
    Binding* binding = vm->bindings;
    while (binding) {
        Binding* next = binding->next;
        if (!object || binding->object == object) {
            unbind(vm, binding);
        }
        binding = next;
    }
}

// Every binding of `object` in each of the file's address spaces.
void unbindObject(DrmFile* file, GemObject* object) {
    for (DrmVm* vm = file->vms; vm; vm = vm->next) {
        unbindAll(vm, object);
    }
}

void retainVm(DrmVm* vm) {
    ++vm->refs;
}

// The last reference: nothing can run in it any more (contexts hold one,
// and a context is freed only once its requests have retired).
void releaseVm(DrmVm* vm) {
    if (--vm->refs != 0) {
        return;
    }
    unbindAll(vm, nullptr);
    if (vm->ppgtt) {
        IntelGen9Driver::get().destroyPpgtt(vm->ppgtt);
    }
    DrmVm** link = &vm->owner->vms;
    while (*link && *link != vm) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = vm->next;
    }
    delete vm;
}

DrmContext* newContext(DrmFile* file) {
    auto* context = new DrmContext();
    if (!context) {
        return nullptr;
    }
    context->vm = file->defaultVm;
    retainVm(context->vm);
    return context;
}

void freeContext(DrmContext* context) {
    for (HwContext& hw : context->engines) {
        if (hw.image || hw.ring) {
            IntelGen9Driver::get().destroyHwContext(hw);
        }
    }
    if (context->timeline) {
        context->timeline->release();
    }
    if (context->vm) {
        releaseVm(context->vm);
    }
    delete context;
}

void destroyFile(DrmFile* file) {
    for (DrmContext*& context : file->contexts) {
        if (context) {
            freeContext(context);
            context = nullptr;
        }
    }
    for (DrmVm*& vm : file->vmIds) {
        if (vm) {
            releaseVm(vm);
            vm = nullptr;
        }
    }
    if (file->defaultVm) {
        releaseVm(file->defaultVm);
    }
    for (uint32_t i = 0; i < file->handleCapacity; ++i) {
        if (file->handles[i]) {
            file->handles[i]->release();
        }
    }
    if (file->handles) {
        kfree(file->handles);
    }
    for (uint32_t i = 0; i < file->syncobjCapacity; ++i) {
        if (file->syncobjs[i]) {
            releaseSync(file->syncobjs[i]);
        }
    }
    if (file->syncobjs) {
        kfree(file->syncobjs);
    }
    delete file;
}

// The CPU caches of an object the GPU does not snoop (I915_CACHING_NONE):
// written back before the GPU reads, dropped before the CPU reads.
void flushObject(const GemObject* object) {
    for (uint32_t page = 0; page < object->pageCount; ++page) {
        auto* bytes = reinterpret_cast<uint8_t*>(VMM::PhysToVirt(object->pages[page]));
        for (uint64_t line = 0; line < kPage; line += 64) {
            asm volatile("clflush (%0)" :: "r"(bytes + line) : "memory");
        }
    }
    asm volatile("mfence" ::: "memory");
}

bool callerPrivileged() {
    Process* current = Scheduler::get().getCurrentProcess();
    return current && current->isPrivileged();
}

// Sleeps (the big kernel lock released) until `done()` or the deadline
// (uptime in ns; kForever), moving the GPU's requests along meanwhile: a
// waiter is what notices a hang. False on the deadline.
template <typename Done>
bool waitFor(Done done, uint64_t deadlineNs) {
    IntelGen9Driver& gpu = IntelGen9Driver::get();
    for (;;) {
        const uint64_t seen = gpu.gpuEvents();
        if (gpuPresent()) {
            gpu.processRequests();
        }
        if (done()) {
            return true;
        }
        const uint64_t now = time_get_uptime_ns();
        if (now >= deadlineNs) {
            return false;
        }
        uint64_t ms = (deadlineNs - now + 999999) / 1000000;
        if (ms > 10) {
            ms = 10;
        }
        gpu.sleepForGpu(seen, ms);
    }
}

Syncobj* lookupSyncobj(DrmFile* file, uint32_t handle) {
    if (handle == 0 || handle > file->syncobjCapacity) {
        return nullptr;
    }
    return file->syncobjs[handle - 1];
}

Fence* signaledFence() {
    auto* fence = new Fence();
    if (fence) {
        fence->signaled = true;
    }
    return fence;
}

// One fence for all of `fences` (the caller keeps its references): the only
// one still pending, a join of those pending (failed ones kept, so the error
// shows), or a fresh signaled fence when all are done. Joins are flattened.
// nullptr only when out of memory.
Fence* joinFences(Fence* const* fences, uint32_t count) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (fences[i]) {
            total += fences[i]->parts ? fences[i]->partCount : 1;
        }
    }
    Fence** pending = total ? new Fence*[total] : nullptr;
    if (total && !pending) {
        return nullptr;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        Fence* f = fences[i];
        if (!f) {
            continue;
        }
        const uint32_t parts = f->parts ? f->partCount : 1;
        for (uint32_t k = 0; k < parts; ++k) {
            Fence* part = f->parts ? f->parts[k] : f;
            if (part->poll() && part->status == 0) {
                continue;   // done, nothing to carry
            }
            bool seen = false;
            for (uint32_t j = 0; j < n && !seen; ++j) {
                seen = pending[j] == part;
            }
            if (!seen) {
                pending[n++] = part;
            }
        }
    }
    if (n == 0) {
        delete[] pending;
        return signaledFence();
    }
    if (n == 1) {
        Fence* only = pending[0];
        delete[] pending;
        only->retain();
        return only;
    }
    auto* join = new Fence();
    if (!join) {
        delete[] pending;
        return nullptr;
    }
    for (uint32_t j = 0; j < n; ++j) {
        pending[j]->retain();
    }
    join->parts = pending;
    join->partCount = n;
    return join;
}

// Drops the points at the front that are signaled without an error.
void prunePoints(Syncobj* sync) {
    uint32_t drop = 0;
    while (drop < sync->pointCount && sync->points[drop].fence->poll() && sync->points[drop].fence->status == 0) {
        sync->signaledPoint = sync->points[drop].point;
        sync->points[drop].fence->release();
        ++drop;
    }
    if (drop == 0) {
        return;
    }
    for (uint32_t i = drop; i < sync->pointCount; ++i) {
        sync->points[i - drop] = sync->points[i];
    }
    sync->pointCount -= drop;
}

// The fence to wait on for `point` (0: the payload), a new reference;
// nullptr when that point has not been submitted yet (or out of memory).
Fence* syncFenceFor(Syncobj* sync, uint64_t point) {
    if (point == 0) {
        if (sync->fence) {
            sync->fence->retain();
        }
        return sync->fence;
    }
    prunePoints(sync);
    uint32_t upTo = 0;
    while (upTo < sync->pointCount && sync->points[upTo].point < point) {
        ++upTo;
    }
    if (upTo == sync->pointCount) {
        // Not there yet, unless it was reached and dropped already.
        return point <= sync->signaledPoint || point <= sync->lastPoint ? signaledFence() : nullptr;
    }
    Fence** list = new Fence*[upTo + 1];
    if (!list) {
        return nullptr;
    }
    for (uint32_t i = 0; i <= upTo; ++i) {
        list[i] = sync->points[i].fence;
    }
    Fence* fence = joinFences(list, upTo + 1);
    delete[] list;
    return fence;
}

// A new point (TIMELINE_SIGNAL, execbuffer, TRANSFER, an imported
// sync_file); the payload becomes the newest point's fence.
bool syncAddPoint(Syncobj* sync, uint64_t point, Fence* fence) {
    prunePoints(sync);
    uint32_t at = 0;
    while (at < sync->pointCount && sync->points[at].point < point) {
        ++at;
    }
    if (at < sync->pointCount && sync->points[at].point == point) {
        fence->retain();
        sync->points[at].fence->release();
        sync->points[at].fence = fence;
    } else {
        if (sync->pointCount == sync->pointCapacity) {
            const uint32_t capacity = sync->pointCapacity ? sync->pointCapacity * 2 : 8;
            auto* grown = static_cast<TimelinePoint*>(kmalloc(sizeof(TimelinePoint) * capacity));
            if (!grown) {
                return false;
            }
            for (uint32_t i = 0; i < sync->pointCount; ++i) {
                grown[i] = sync->points[i];
            }
            if (sync->points) {
                kfree(sync->points);
            }
            sync->points = grown;
            sync->pointCapacity = capacity;
        }
        for (uint32_t i = sync->pointCount; i > at; --i) {
            sync->points[i] = sync->points[i - 1];
        }
        fence->retain();
        sync->points[at] = TimelinePoint{point, fence};
        ++sync->pointCount;
    }
    if (point > sync->lastPoint) {
        sync->lastPoint = point;
    }
    Fence* head = syncFenceFor(sync, sync->lastPoint);
    if (head) {
        setFence(sync->fence, head);
        head->release();
    }
    return true;
}

// The binary payload (point 0): replaces whatever the sync object held,
// timeline points included (Linux's drm_syncobj_replace_fence).
void syncSetPayload(Syncobj* sync, Fence* fence) {
    clearPoints(sync);
    setFence(sync->fence, fence);
}

bool syncSignal(Syncobj* sync, uint64_t point, Fence* fence) {
    if (point == 0) {
        syncSetPayload(sync, fence);
        return true;
    }
    return syncAddPoint(sync, point, fence);
}

// The highest point reached: every point up to it signaled.
uint64_t syncQuery(Syncobj* sync) {
    prunePoints(sync);
    uint64_t reached = sync->signaledPoint;
    for (uint32_t i = 0; i < sync->pointCount && sync->points[i].fence->poll(); ++i) {
        reached = sync->points[i].point;
    }
    return reached;
}

// --- fds: dma-bufs, sync_files, sync object fds ---------------------------
//
// In the DRM structs an fd is the slot of a File handle (the handle without
// its type bits: it must fit 32 bits); `(1 << 48) | slot` is the handle.

int fenceNodeClose(VNode* node) {
    auto* fence = static_cast<Fence*>(node->getData());
    node->setData(nullptr);
    if (fence) {
        fence->release();
    }
    return 0;
}

int syncobjNodeClose(VNode* node) {
    auto* sync = static_cast<Syncobj*>(node->getData());
    node->setData(nullptr);
    if (sync) {
        releaseSync(sync);
    }
    return 0;
}

int dmaBufNodeClose(VNode* node) {
    auto* object = static_cast<GemObject*>(node->getData());
    node->setData(nullptr);
    if (object) {
        object->release();
    }
    return 0;
}

int anonNodeStat(VNode* node, FileStats* stats) {
    if (!stats) {
        return -1;
    }
    memset(stats, 0, sizeof(*stats));
    stats->type = node->getType();
    stats->mode = 0600;
    stats->links = 1;
    return 0;
}

// A dma-buf's size is its object's (lseek(fd, 0, SEEK_END), as Mesa reads it).
int dmaBufNodeStat(VNode* node, FileStats* stats) {
    if (anonNodeStat(node, stats) != 0) {
        return -1;
    }
    stats->mode = 0100600;
    auto* object = static_cast<GemObject*>(node->getData());
    stats->size = object ? object->size : 0;
    return 0;
}

VNodeOps gSyncFileOps = {nullptr, fenceNodeClose, nullptr, nullptr, anonNodeStat};
VNodeOps gSyncobjFdOps = {nullptr, syncobjNodeClose, nullptr, nullptr, anonNodeStat};
VNodeOps gDmaBufOps = {nullptr, dmaBufNodeClose, nullptr, nullptr, dmaBufNodeStat};

// A node for `data` (whose reference it takes over).
VNode* makeNode(VNodeOps* ops, void* data, FileType type) {
    auto* node = new VNode(nullptr, 0, type);
    if (!node) {
        return nullptr;
    }
    node->refCount = 0;
    node->ops = ops;
    node->setData(data);
    return node;
}

// Gives the calling process an fd for `node` (closing it on failure); the
// slot, or -errno.
int64_t installNode(VNode* node, bool closeOnExec) {
    Process* current = Scheduler::get().getCurrentProcess();
    auto* fd = current ? new FileDescriptor(node, 2 /* O_RDWR */) : nullptr;
    if (!fd) {
        node->ops->close(node);
        delete node;
        return -kENOMEM;
    }
    const uint64_t handle = current->allocateFD(fd, HandleRightRead | HandleRightWrite | HandleRightDuplicate, closeOnExec);
    if (handle == static_cast<uint64_t>(-1)) {
        VFS::get().close(fd);
        return -kEMFILE;
    }
    return static_cast<int64_t>(handle & HandleTable::SlotMask);
}

// The node behind the calling process's fd `slot` if it has these ops.
VNode* nodeOfFd(int32_t slot, const VNodeOps* ops) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || slot < 0) {
        return nullptr;
    }
    HandleEntry* entry = current->getHandle(HandleTable::encodeHandle(HandleType::File, slot));
    if (!entry || entry->type != HandleType::File) {
        return nullptr;
    }
    auto* fd = reinterpret_cast<FileDescriptor*>(entry->object);
    VNode* node = fd ? fd->getNode() : nullptr;
    return node && node->ops == ops && node->getData() ? node : nullptr;
}

// A sync_file fd holding `fence` (a new reference is taken).
int64_t exportSyncFile(Fence* fence, bool closeOnExec) {
    fence->retain();
    VNode* node = makeNode(&gSyncFileOps, fence, FileType::CharDevice);
    if (!node) {
        fence->release();
        return -kENOMEM;
    }
    return installNode(node, closeOnExec);
}

Fence* syncFileFence(int32_t slot) {
    VNode* node = nodeOfFd(slot, &gSyncFileOps);
    return node ? static_cast<Fence*>(node->getData()) : nullptr;
}

int nodeClose(VNode* node) {
    DrmFile* file = fileOf(node);
    if (file) {
        node->setData(nullptr);
        file->closed = true;
        if (file->inflight == 0) {
            destroyFile(file);
        }   // else the last execbuffer to return destroys it
    }
    return 0;
}

int nodeStat(VNode*, FileStats* stats) {
    if (!stats) {
        return -1;
    }
    memset(stats, 0, sizeof(*stats));
    stats->type = FileType::CharDevice;
    stats->mode = 0020000 | 0666;
    stats->links = 1;
    stats->rdev = (226u << 8) | 128u;   // Linux's DRM major, render minor 128
    return 0;
}

VNodeOps gRenderNodeOps = {
    nullptr,     // open
    nodeClose,
    nullptr,     // read
    nullptr,     // write
    nodeStat,
};

// Copies a user struct of `size` bytes (the ioctl's encoded size) in, zero
// filling a shorter one, as drm_ioctl does.
template <typename T>
bool copyIn(T* out, uint64_t arg, uint32_t size) {
    memset(out, 0, sizeof(T));
    const uint32_t n = size < sizeof(T) ? size : static_cast<uint32_t>(sizeof(T));
    return n == 0 || Syscall::copyFromUser(out, arg, n);
}

template <typename T>
bool copyOut(uint64_t arg, const T* in, uint32_t size) {
    const uint32_t n = size < sizeof(T) ? size : static_cast<uint32_t>(sizeof(T));
    return n == 0 || Syscall::copyToUser(arg, in, n);
}

uint64_t copyString(uint64_t user, uint64_t* length, const char* text) {
    const uint64_t textLength = strlen(text);
    if (user != 0 && *length != 0) {
        const uint64_t n = *length < textLength ? *length : textLength;
        if (!Syscall::copyToUser(user, text, n)) {
            return err(kEFAULT);
        }
    }
    *length = textLength;
    return 0;
}

uint64_t doVersion(uint64_t arg, uint32_t size) {
    drm_version version;
    if (!copyIn(&version, arg, size)) {
        return err(kEFAULT);
    }
    // What Linux's i915 reports (DRIVER_MAJOR/MINOR/PATCHLEVEL, DATE, DESC).
    version.version_major = 1;
    version.version_minor = 6;
    version.version_patchlevel = 0;
    uint64_t r = copyString(version.name, &version.name_len, "i915");
    if (r == 0) r = copyString(version.date, &version.date_len, "20201103");
    if (r == 0) r = copyString(version.desc, &version.desc_len, "Intel Graphics");
    if (r != 0) {
        return r;
    }
    return copyOut(arg, &version, size) ? 0 : err(kEFAULT);
}

// GETPARAM (i915_getparam_ioctl): what i915 answers on Skylake, except
// where this driver does less (no video engines, no command parser, no
// priority scheduler, no userptr, no perf), which it reports as i915 does
// when a part lacks it.
uint64_t doGetparam(uint64_t arg, uint32_t size) {
    drm_i915_getparam request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    int32_t value = 0;
    IntelGen9Driver& gpu = IntelGen9Driver::get();
    switch (request.param) {
        case I915_PARAM_CHIPSET_ID:
            if (!gpu.deviceInfo()) return err(kENODEV);
            value = gpu.deviceInfo()->deviceId;
            break;
        case I915_PARAM_REVISION:
            if (!gpuPresent()) return err(kENODEV);
            value = gpu.pciRevision();
            break;
        case I915_PARAM_HAS_GEM:
        case I915_PARAM_HAS_EXECBUF2:
        case I915_PARAM_HAS_BLT:
        case I915_PARAM_HAS_LLC:
        case I915_PARAM_HAS_RELAXED_FENCING:
        case I915_PARAM_HAS_COHERENT_RINGS:
        case I915_PARAM_HAS_RELAXED_DELTA:
        case I915_PARAM_HAS_WAIT_TIMEOUT:
        case I915_PARAM_HAS_PRIME_VMAP_FLUSH:
        case I915_PARAM_HAS_PINNED_BATCHES:
        case I915_PARAM_HAS_EXEC_NO_RELOC:
        case I915_PARAM_HAS_EXEC_HANDLE_LUT:
        case I915_PARAM_HAS_COHERENT_PHYS_GTT:
        case I915_PARAM_HAS_EXEC_SOFTPIN:
        case I915_PARAM_HAS_EXEC_ASYNC:
        case I915_PARAM_HAS_EXEC_CAPTURE:      // accepted; nothing is captured
        case I915_PARAM_HAS_EXEC_BATCH_FIRST:
        case I915_PARAM_HAS_EXEC_FENCE_ARRAY:
            value = 1;
            break;
        case I915_PARAM_HAS_BSD:
        case I915_PARAM_HAS_BSD2:
        case I915_PARAM_HAS_VEBOX:
        case I915_PARAM_HAS_EXEC_CONSTANTS:
        case I915_PARAM_HAS_GEN7_SOL_RESET:
        case I915_PARAM_HAS_SEMAPHORES:
        case I915_PARAM_HAS_SECURE_BATCHES:
        case I915_PARAM_HAS_WT:
        case I915_PARAM_CMD_PARSER_VERSION:
        case I915_PARAM_HAS_RESOURCE_STREAMER:
        case I915_PARAM_HAS_POOLED_EU:
        case I915_PARAM_MIN_EU_IN_POOL:
        case I915_PARAM_HAS_SCHEDULER:
        case I915_PARAM_MMAP_GTT_COHERENT:
        case I915_PARAM_HAS_EXEC_SUBMIT_FENCE:
        case I915_PARAM_HAS_USERPTR_PROBE:
        case I915_PARAM_HAS_CONTEXT_FREQ_HINT:
            value = 0;
            break;
        case I915_PARAM_HAS_EXEC_FENCE:            // FENCE_IN / FENCE_OUT sync_files
        case I915_PARAM_HAS_EXEC_TIMELINE_FENCES:
            value = 1;
            break;
        case I915_PARAM_HAS_ALIASING_PPGTT:
            value = 3;   // INTEL_PPGTT_FULL: an address space of its own per context's VM
            break;
        case I915_PARAM_HAS_GPU_RESET:
            value = 2;   // engine resets
            break;
        case I915_PARAM_HAS_CONTEXT_ISOLATION:
            value = (1 << I915_ENGINE_CLASS_RENDER) | (1 << I915_ENGINE_CLASS_COPY);
            break;
        case I915_PARAM_MMAP_VERSION:
            value = 1;
            break;
        case I915_PARAM_MMAP_GTT_VERSION:
            value = 4;   // with MMAP_OFFSET
            break;
        case I915_PARAM_CS_TIMESTAMP_FREQUENCY:
            value = static_cast<int32_t>(gpu.timestampFrequency());
            if (value == 0) return err(kENODEV);
            break;
        case I915_PARAM_SLICE_MASK:
        case I915_PARAM_SUBSLICE_MASK:
        case I915_PARAM_SUBSLICE_TOTAL:
        case I915_PARAM_EU_TOTAL: {
            const intel_gen9::Gen9Sseu* sseu = gpuPresent() ? gpu.topology() : nullptr;
            if (!sseu) return err(kENODEV);
            if (request.param == I915_PARAM_SLICE_MASK) {
                value = sseu->sliceMask;
            } else if (request.param == I915_PARAM_SUBSLICE_MASK) {
                value = sseu->subsliceMask;
            } else if (request.param == I915_PARAM_SUBSLICE_TOTAL) {
                value = static_cast<int32_t>(intel_gen9::popcount8(sseu->sliceMask) *
                                             intel_gen9::popcount8(sseu->subsliceMask));
            } else {
                value = static_cast<int32_t>(sseu->euTotal);
            }
            if (value == 0) return err(kENODEV);
            break;
        }
        case I915_PARAM_HUC_STATUS:
        case I915_PARAM_PXP_STATUS:
            return err(kENODEV);   // no HuC, no PXP
        default:
            return err(kEINVAL);   // PERF_REVISION, OA_TIMESTAMP_FREQUENCY (no perf) and unknown ones
    }
    if (request.value == 0 || !Syscall::copyToUser(request.value, &value, sizeof(value))) {
        return err(kEFAULT);
    }
    return 0;
}

uint64_t doCreate(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_create request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    const uint64_t bytes = (request.size + kPage - 1) & ~(kPage - 1);
    if (bytes == 0 || bytes > kMaxObjectBytes) {
        return err(kEINVAL);
    }
    GemObject* object = GemObject::create(bytes);
    if (!object) {
        return err(kENOMEM);
    }
    const uint32_t handle = addHandle(file, object);
    if (handle == 0) {
        object->release();
        return err(kENOMEM);
    }
    request.size = bytes;
    request.handle = handle;
    if (!copyOut(arg, &request, size)) {
        file->handles[handle - 1] = nullptr;
        object->release();
        return err(kEFAULT);
    }
    return 0;
}

uint64_t doClose(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_gem_close request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    GemObject* object = lookup(file, request.handle);
    if (!object) {
        return err(kEINVAL);
    }
    // i915 unbinds the object from this file's address spaces too; not while
    // the GPU may still use it there (i915 defers that, this waits).
    if (!objectIdle(object)) {
        object->retain();
        waitFor([&] { return objectIdle(object); }, kForever);
        const bool still = lookup(file, request.handle) == object;
        object->release();
        if (!still) {
            return err(kEINVAL);   // another thread closed it meanwhile
        }
    }
    unbindObject(file, object);
    file->handles[request.handle - 1] = nullptr;
    object->release();
    return 0;
}

// The mmap offset names the handle and the caching: handle << 32 | mode << 28
// (page aligned, below 2^63 so mmap's off_t takes it); mmap may start at a
// later page of the object by adding to it.
constexpr uint64_t kOffsetModeShift = 28;
constexpr uint64_t kOffsetPageMask = (1ull << kOffsetModeShift) - 1;

uint64_t doMmapOffset(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_mmap_offset request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if (request.extensions != 0 || !lookup(file, request.handle)) {
        return err(request.extensions != 0 ? kEINVAL : kENOENT);
    }
    uint64_t mode = request.flags;
    if (mode == I915_MMAP_OFFSET_FIXED) {
        mode = I915_MMAP_OFFSET_WB;   // on an LLC part, FIXED is write-back
    }
    if (mode == I915_MMAP_OFFSET_GTT) {
        return err(kENODEV);          // no aperture mmaps (tiling fences) yet
    }
    if (mode != I915_MMAP_OFFSET_WB && mode != I915_MMAP_OFFSET_WC && mode != I915_MMAP_OFFSET_UC) {
        return err(kEINVAL);
    }
    request.offset = static_cast<uint64_t>(request.handle) << 32 | mode << kOffsetModeShift;
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// One context parameter (CONTEXT_SETPARAM, or a SETPARAM extension of
// CONTEXT_CREATE_EXT), as i915_gem_context.c checks it; 0 or -errno.
int64_t setContextParam(DrmFile* file, DrmContext* context, const drm_i915_gem_context_param& p) {
    const bool scalar = p.param != I915_CONTEXT_PARAM_ENGINES && p.param != I915_CONTEXT_PARAM_SSEU &&
                        p.param != I915_CONTEXT_PARAM_CONTEXT_IMAGE;
    if (scalar && p.size != 0) {
        return -kEINVAL;
    }
    switch (p.param) {
        case I915_CONTEXT_PARAM_NO_ZEROMAP:
            context->noZeromap = p.value != 0;
            return 0;
        case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE:
            context->noErrorCapture = p.value != 0;
            return 0;
        case I915_CONTEXT_PARAM_BANNABLE:
            if (p.value == 0 && !callerPrivileged()) {
                return -kEPERM;   // only the privileged may keep a context from being banned
            }
            context->bannable = p.value != 0;
            return 0;
        case I915_CONTEXT_PARAM_RECOVERABLE:
            context->recoverable = p.value != 0;
            return 0;
        case I915_CONTEXT_PARAM_PRIORITY: {
            // Requests run in submission order (no priority scheduler, which
            // i915 reports as ENODEV); the default priority is what they get.
            const int64_t priority = static_cast<int64_t>(p.value);
            if (priority > I915_CONTEXT_MAX_USER_PRIORITY || priority < I915_CONTEXT_MIN_USER_PRIORITY) {
                return -kEINVAL;
            }
            if (priority != 0) {
                return -kENODEV;
            }
            context->priority = 0;
            return 0;
        }
        case I915_CONTEXT_PARAM_VM: {
            if (context->used) {
                return -kEINVAL;   // fixed once it has run
            }
            DrmVm* vm = lookupVm(file, p.value);
            if (!vm) {
                return -kENOENT;
            }
            retainVm(vm);
            releaseVm(context->vm);
            context->vm = vm;
            return 0;
        }
        case I915_CONTEXT_PARAM_ENGINES: {
            if (context->used) {
                return -kEINVAL;
            }
            if (p.size == 0) {
                context->engineMapCount = 0;   // back to the legacy selectors
                return 0;
            }
            if (p.size < sizeof(i915_context_param_engines) ||
                (p.size - sizeof(i915_context_param_engines)) % sizeof(i915_engine_class_instance) != 0) {
                return -kEINVAL;
            }
            const uint64_t count = (p.size - sizeof(i915_context_param_engines)) / sizeof(i915_engine_class_instance);
            if (count > kMaxEngineMap) {
                return -kEINVAL;
            }
            i915_context_param_engines header;
            i915_engine_class_instance map[kMaxEngineMap];
            if (!Syscall::copyFromUser(&header, p.value, sizeof(header)) ||
                (count && !Syscall::copyFromUser(map, p.value + sizeof(header), count * sizeof(map[0])))) {
                return -kEFAULT;
            }
            if (header.extensions != 0) {
                return -kENODEV;   // load balancing, bonding, parallel submission: no
            }
            for (uint64_t i = 0; i < count; ++i) {
                const bool none = map[i].engine_class == I915_ENGINE_CLASS_INVALID_NONE &&
                                  map[i].engine_instance == I915_ENGINE_CLASS_INVALID_NONE;
                const bool known = map[i].engine_instance == 0 && (map[i].engine_class == I915_ENGINE_CLASS_RENDER ||
                                                                   map[i].engine_class == I915_ENGINE_CLASS_COPY);
                if (!none && !known) {
                    return -kENOENT;   // no such engine
                }
            }
            for (uint64_t i = 0; i < count; ++i) {
                context->engineMap[i] = map[i];
            }
            context->engineMapCount = static_cast<uint32_t>(count);
            return 0;
        }
        case I915_CONTEXT_PARAM_PERSISTENCE:
            // Requests always run to the end after their context is closed;
            // cancelling them (non-persistent) is not supported.
            return p.value != 0 ? 0 : -kENODEV;
        case I915_CONTEXT_PARAM_RINGSIZE:
            return p.value == 16 * 1024 ? 0 : -kEINVAL;   // the rings are 16 KiB
        case I915_CONTEXT_PARAM_PROTECTED_CONTENT:
            return p.value == 0 ? 0 : -kENODEV;           // no PXP
        case I915_CONTEXT_PARAM_SSEU:
        case I915_CONTEXT_PARAM_LOW_LATENCY:
            return -kENODEV;
        default:
            return -kEINVAL;   // BAN_PERIOD (gone from i915), GTT_SIZE (read only), CONTEXT_IMAGE, unknown
    }
}

int64_t getContextParam(DrmFile* file, DrmContext* context, drm_i915_gem_context_param& p) {
    switch (p.param) {
        case I915_CONTEXT_PARAM_NO_ZEROMAP:
            p.value = context->noZeromap;
            break;
        case I915_CONTEXT_PARAM_GTT_SIZE:
            p.value = kVaLimit;
            break;
        case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE:
            p.value = context->noErrorCapture;
            break;
        case I915_CONTEXT_PARAM_BANNABLE:
            p.value = context->bannable;
            break;
        case I915_CONTEXT_PARAM_RECOVERABLE:
            p.value = context->recoverable;
            break;
        case I915_CONTEXT_PARAM_PRIORITY:
            p.value = static_cast<uint64_t>(context->priority);
            break;
        case I915_CONTEXT_PARAM_PERSISTENCE:
            p.value = 1;
            break;
        case I915_CONTEXT_PARAM_RINGSIZE:
            p.value = 16 * 1024;
            break;
        case I915_CONTEXT_PARAM_PROTECTED_CONTENT:
            p.value = 0;
            break;
        case I915_CONTEXT_PARAM_VM: {
            // A VM id for the context's address space (i915 gives one out).
            DrmVm* vm = context->vm;
            if (vm->id == 0) {
                uint32_t slot = 0;
                while (slot < kMaxVms && file->vmIds[slot]) {
                    ++slot;
                }
                if (slot == kMaxVms) {
                    return -kENOSPC;
                }
                retainVm(vm);
                file->vmIds[slot] = vm;
                vm->id = slot + 1;
            }
            p.value = vm->id;
            break;
        }
        case I915_CONTEXT_PARAM_ENGINES: {
            const uint32_t bytes = context->engineMapCount == 0
                                       ? 0
                                       : static_cast<uint32_t>(sizeof(i915_context_param_engines) +
                                                               context->engineMapCount * sizeof(i915_engine_class_instance));
            if (p.size == 0 || bytes == 0) {
                p.size = bytes;
                return 0;
            }
            if (p.size < bytes) {
                return -kEINVAL;
            }
            const i915_context_param_engines header = {0};
            if (!Syscall::copyToUser(p.value, &header, sizeof(header)) ||
                !Syscall::copyToUser(p.value + sizeof(header), context->engineMap,
                                     context->engineMapCount * sizeof(i915_engine_class_instance))) {
                return -kEFAULT;
            }
            p.size = bytes;
            return 0;
        }
        default:
            return -kEINVAL;   // SSEU, LOW_LATENCY, BAN_PERIOD, unknown
    }
    p.size = 0;
    return 0;
}

// CONTEXT_CREATE and CONTEXT_CREATE_EXT (the same number, a larger struct):
// a new context in the file's default address space; CREATE_EXT's chain of
// SETPARAM extensions sets its parameters first (i915's proto-context).
uint64_t doContextCreate(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_context_create_ext request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    const bool ext = size >= sizeof(request);
    const uint32_t known = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS | I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE;
    if (ext && (request.flags & ~known) != 0) {
        return err(kEINVAL);
    }
    if (ext && !(request.flags & I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS) && request.extensions != 0) {
        return err(kEINVAL);
    }
    uint32_t id = 1;
    while (id < kMaxContexts && file->contexts[id]) {
        ++id;
    }
    if (id == kMaxContexts) {
        return err(kENOSPC);
    }
    DrmContext* context = newContext(file);
    if (!context) {
        return err(kENOMEM);
    }
    context->singleTimeline = ext && (request.flags & I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE);
    int64_t result = 0;
    uint64_t next = ext && (request.flags & I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS) ? request.extensions : 0;
    for (uint32_t links = 0; next != 0 && result == 0; ++links) {
        drm_i915_gem_context_create_ext_setparam block;
        if (links == 64) {
            result = -kEINVAL;   // a chain that long is a loop
        } else if (!Syscall::copyFromUser(&block.base, next, sizeof(block.base))) {
            result = -kEFAULT;
        } else if (block.base.flags != 0 || block.base.rsvd[0] || block.base.rsvd[1] || block.base.rsvd[2] ||
                   block.base.rsvd[3]) {
            result = -kEINVAL;
        } else if (block.base.name != I915_CONTEXT_CREATE_EXT_SETPARAM) {
            result = -kEINVAL;   // CLONE is gone from i915; nothing else exists
        } else if (!Syscall::copyFromUser(&block, next, sizeof(block))) {
            result = -kEFAULT;
        } else if (block.param.ctx_id != 0) {
            result = -kEINVAL;
        } else {
            result = setContextParam(file, context, block.param);
        }
        next = block.base.next_extension;
    }
    if (result != 0) {
        freeContext(context);
        return err(-result);
    }
    file->contexts[id] = context;
    request.ctx_id = id;
    if (!copyOut(arg, &request, size)) {
        file->contexts[id] = nullptr;
        freeContext(context);
        return err(kEFAULT);
    }
    return 0;
}

uint64_t doContextParam(DrmFile* file, uint64_t arg, uint32_t size, bool set) {
    drm_i915_gem_context_param request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if (request.ctx_id >= kMaxContexts || !file->contexts[request.ctx_id]) {
        return err(kENOENT);
    }
    DrmContext* context = file->contexts[request.ctx_id];
    const int64_t result = set ? setContextParam(file, context, request) : getContextParam(file, context, request);
    if (result != 0) {
        return err(-result);
    }
    return set || copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// GEM_VM_CREATE / GEM_VM_DESTROY (i915_gem_vm_create_ioctl): an address
// space by id, for CONTEXT_PARAM_VM. Destroying the id leaves it to the
// contexts still using it.
uint64_t doVmCreate(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_vm_control request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if (request.flags != 0 || request.extensions != 0 || request.vm_id != 0) {
        return err(kEINVAL);
    }
    uint32_t slot = 0;
    while (slot < kMaxVms && file->vmIds[slot]) {
        ++slot;
    }
    if (slot == kMaxVms) {
        return err(kENOSPC);
    }
    DrmVm* vm = createVm(file);
    if (!vm) {
        return err(kENOMEM);
    }
    file->vmIds[slot] = vm;   // the table's reference is the one it was made with
    vm->id = slot + 1;
    request.vm_id = vm->id;
    if (!copyOut(arg, &request, size)) {
        file->vmIds[slot] = nullptr;
        vm->id = 0;
        releaseVm(vm);
        return err(kEFAULT);
    }
    return 0;
}

uint64_t doVmDestroy(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_vm_control request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if (request.flags != 0 || request.extensions != 0) {
        return err(kEINVAL);
    }
    DrmVm* vm = lookupVm(file, request.vm_id);
    if (!vm) {
        return err(kENOENT);
    }
    file->vmIds[request.vm_id - 1] = nullptr;
    vm->id = 0;
    releaseVm(vm);
    return 0;
}

uint64_t doContextDestroy(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_context_destroy request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    // The default context cannot be destroyed (i915 says ENOENT too).
    if (request.ctx_id == 0 || request.ctx_id >= kMaxContexts || !file->contexts[request.ctx_id]) {
        return err(kENOENT);
    }
    DrmContext* context = file->contexts[request.ctx_id];
    file->contexts[request.ctx_id] = nullptr;
    if (context->pending != 0) {
        context->destroyed = true;   // its last request frees it
    } else {
        freeContext(context);
    }
    return 0;
}

uint64_t doHandleOnly(DrmFile* file, uint32_t handle) {
    return lookup(file, handle) ? 0 : err(kENOENT);
}

// What a queued execbuffer holds until it retires.
struct GemRequest {
    Request rq;
    DrmFile* file;
    DrmContext* context;
    GemObject** objects;    // referenced
    uint32_t objectCount;
};

// Called once the request's fence is signaled (from the GPU's interrupt or
// a waiter): lets go of everything; the file and the context go too when
// they were closed or destroyed meanwhile.
void retireGemRequest(Request* request) {
    auto* g = static_cast<GemRequest*>(request->owner);
    DrmContext* context = g->context;
    DrmFile* file = g->file;
    if (request->hung) {
        // i915's context_mark_guilty: a context that cannot recover is
        // banned (when it may be); a recoverable one carries on from the
        // default state (its image was lost with the reset).
        ++context->guilty;
        if (!context->recoverable && context->bannable) {
            context->banned = true;
        }
    }
    for (uint32_t i = 0; i < g->objectCount; ++i) {
        g->objects[i]->release();
    }
    kfree(g->objects);
    for (uint32_t i = 0; i < request->depCount; ++i) {
        request->deps[i]->release();
    }
    if (request->deps) {
        kfree(request->deps);
    }
    request->done->release();
    delete g;
    if (--context->pending == 0 && context->destroyed) {
        freeContext(context);
    }
    if (--file->inflight == 0 && file->closed) {
        destroyFile(file);
    }
}

void addDependency(Fence** deps, uint32_t& count, Fence* fence) {
    if (!fence || fence->poll()) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (deps[i] == fence) {
            return;
        }
    }
    fence->retain();
    deps[count++] = fence;
}

// Which engine a request goes to: through the context's engine map when it
// has one (the ring field indexes it), else the legacy selectors.
bool pickEngine(const DrmContext* context, uint64_t ring, EngineId* engine) {
    if (context && context->engineMapCount != 0) {
        if (ring >= context->engineMapCount) {
            return false;
        }
        const uint16_t engineClass = context->engineMap[ring].engine_class;
        if (engineClass == I915_ENGINE_CLASS_RENDER) {
            *engine = EngineId::Render;
            return true;
        }
        if (engineClass == I915_ENGINE_CLASS_COPY) {
            *engine = EngineId::Blitter;
            return true;
        }
        return false;   // an empty slot
    }
    if (ring == I915_EXEC_DEFAULT || ring == I915_EXEC_RENDER) {
        *engine = EngineId::Render;
        return true;
    }
    if (ring == I915_EXEC_BLT) {
        *engine = EngineId::Blitter;
        return true;
    }
    return false;
}

// EXECBUFFER2 with every object softpinned, in the context's address space.
// Each object is bound where the caller placed it (what is in the way is
// unbound first, once the file's requests are done with it, as i915
// evicts), and a request is queued on the engine that runs once what it
// depends on is done: implicitly, the last writer of each object it uses
// and, for an object it writes, the last user on each engine (not for
// EXEC_OBJECT_ASYNC); explicitly, the sync objects of the fence array
// marked WAIT; for a single-timeline context, its previous request. Its
// fence goes into the objects and the sync objects marked SIGNAL. The call
// returns without waiting.
// Execbuffer's extension chain: only TIMELINE_FENCES, once.
uint64_t readExecExtensions(uint64_t next, drm_i915_gem_execbuffer_ext_timeline_fences* timeline, bool* have) {
    for (uint32_t links = 0; next != 0; ++links) {
        i915_user_extension base;
        if (links == 16) {
            return err(kEINVAL);
        }
        if (!Syscall::copyFromUser(&base, next, sizeof(base))) {
            return err(kEFAULT);
        }
        if (base.flags != 0 || base.rsvd[0] || base.rsvd[1] || base.rsvd[2] || base.rsvd[3] ||
            base.name != DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES || *have) {
            return err(kEINVAL);
        }
        if (!Syscall::copyFromUser(timeline, next, sizeof(*timeline))) {
            return err(kEFAULT);
        }
        if (timeline->fence_count > kMaxExecFences) {
            return err(kEINVAL);
        }
        *have = true;
        next = base.next_extension;
    }
    return 0;
}

uint64_t doExecbuffer(DrmFile* file, uint64_t arg, uint32_t size, bool canWriteBack) {
    drm_i915_gem_execbuffer2 exec;
    if (!copyIn(&exec, arg, size)) {
        return err(kEFAULT);
    }
    IntelGen9Driver& gpu = IntelGen9Driver::get();
    if (!gpuPresent()) {
        return err(kENODEV);
    }
    const uint64_t ring = exec.flags & I915_EXEC_RING_MASK;
    const uint64_t knownFlags = I915_EXEC_RING_MASK | I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT |
                                I915_EXEC_BATCH_FIRST | I915_EXEC_FENCE_ARRAY | I915_EXEC_FENCE_IN |
                                I915_EXEC_FENCE_OUT | I915_EXEC_USE_EXTENSIONS;
    const bool fenceArray = (exec.flags & I915_EXEC_FENCE_ARRAY) != 0;
    const bool useExtensions = (exec.flags & I915_EXEC_USE_EXTENSIONS) != 0;
    const bool fenceOut = (exec.flags & I915_EXEC_FENCE_OUT) != 0;
    const uint32_t contextId = static_cast<uint32_t>(exec.rsvd1);   // i915: the lower half
    if (contextId >= kMaxContexts || !file->contexts[contextId]) {
        return err(kENOENT);
    }
    EngineId engine = EngineId::Render;
    if ((exec.flags & ~knownFlags) != 0 || (!fenceArray && exec.num_cliprects != 0) ||
        (fenceArray && exec.num_cliprects > kMaxExecFences) || (fenceArray && useExtensions) ||
        (fenceOut && !canWriteBack) || !pickEngine(file->contexts[contextId], ring, &engine)) {
        return err(kEINVAL);   // FENCE_OUT needs EXECBUFFER2_WR, which hands rsvd2 back
    }
    drm_i915_gem_execbuffer_ext_timeline_fences timeline = {};
    bool haveTimeline = false;
    if (useExtensions) {
        const uint64_t bad = readExecExtensions(exec.cliprects_ptr, &timeline, &haveTimeline);
        if (bad != 0) {
            return bad;
        }
    }
    // FENCE_IN: a sync_file the batch waits for (held until it is a dependency).
    Fence* inFence = nullptr;
    if (exec.flags & I915_EXEC_FENCE_IN) {
        inFence = syncFileFence(static_cast<int32_t>(exec.rsvd2 & 0xFFFFFFFFu));
        if (!inFence) {
            return err(kEINVAL);
        }
        inFence->retain();
    }
    if (exec.buffer_count == 0 || exec.buffer_count > kMaxExecObjects) {
        return err(kEINVAL);
    }
    const uint32_t fenceCount = fenceArray ? exec.num_cliprects : haveTimeline ? static_cast<uint32_t>(timeline.fence_count) : 0;
    const uint64_t listBytes = sizeof(drm_i915_gem_exec_object2) * exec.buffer_count;
    auto* list = static_cast<drm_i915_gem_exec_object2*>(kmalloc(listBytes));
    auto* objects = static_cast<GemObject**>(kmalloc(sizeof(GemObject*) * exec.buffer_count));
    auto* fences = fenceCount ? static_cast<drm_i915_gem_exec_fence*>(kmalloc(sizeof(drm_i915_gem_exec_fence) * fenceCount))
                              : nullptr;
    auto* points = haveTimeline && fenceCount ? static_cast<uint64_t*>(kmalloc(sizeof(uint64_t) * fenceCount)) : nullptr;
    if (!list || !objects || (fenceCount && !fences) || (haveTimeline && fenceCount && !points)) {
        if (list) kfree(list);
        if (objects) kfree(objects);
        if (fences) kfree(fences);
        if (points) kfree(points);
        if (inFence) inFence->release();
        return err(kENOMEM);
    }
    uint64_t result = 0;
    uint32_t held = 0;
    if (!Syscall::copyFromUser(list, exec.buffers_ptr, listBytes) ||
        (fenceCount && !Syscall::copyFromUser(fences, fenceArray ? exec.cliprects_ptr : timeline.handles_ptr,
                                              sizeof(drm_i915_gem_exec_fence) * fenceCount)) ||
        (points && !Syscall::copyFromUser(points, timeline.values_ptr, sizeof(uint64_t) * fenceCount))) {
        result = err(kEFAULT);
    }
    auto pointOf = [&](uint32_t i) { return points ? points[i] : 0; };
    for (uint32_t i = 0; result == 0 && i < fenceCount; ++i) {
        if ((fences[i].flags & ~(I915_EXEC_FENCE_WAIT | I915_EXEC_FENCE_SIGNAL)) != 0) {
            result = err(kEINVAL);
        }
    }
    // Check every entry and take a reference before anything changes.
    const uint64_t knownObjectFlags = EXEC_OBJECT_NEEDS_FENCE | EXEC_OBJECT_NEEDS_GTT | EXEC_OBJECT_WRITE |
                                      EXEC_OBJECT_SUPPORTS_48B_ADDRESS | EXEC_OBJECT_PINNED | EXEC_OBJECT_ASYNC |
                                      EXEC_OBJECT_CAPTURE;
    for (uint32_t i = 0; result == 0 && i < exec.buffer_count; ++i) {
        drm_i915_gem_exec_object2& entry = list[i];
        GemObject* object = lookup(file, entry.handle);
        const uint64_t va = entry.offset & (kVaLimit - 1);   // canonical form -> 48 bits
        if (!object) {
            result = err(kENOENT);
        } else if (!(entry.flags & EXEC_OBJECT_PINNED) || (entry.flags & ~knownObjectFlags) != 0 ||
                   entry.relocation_count != 0 || (va & (kPage - 1)) != 0 || va + object->size > kVaLimit ||
                   va + object->size < va) {
            result = err(kEINVAL);   // relocations are not supported: softpin only
        }
        for (uint32_t j = 0; result == 0 && j < held; ++j) {
            const uint64_t other = list[j].offset & (kVaLimit - 1);
            if (objects[j] == object || (va < other + objects[j]->size && other < va + object->size)) {
                result = err(kEINVAL);   // listed twice, or two objects overlap
            }
        }
        if (result == 0) {
            object->retain();
            objects[held++] = object;
        }
    }
    const uint32_t batchIndex = (exec.flags & I915_EXEC_BATCH_FIRST) ? 0 : exec.buffer_count - 1;
    if (result == 0 && (exec.batch_start_offset % 8 != 0 || exec.batch_start_offset >= objects[batchIndex]->size)) {
        result = err(kEINVAL);
    }
    // The context's address space, kept across the waits below.
    DrmVm* vm = file->contexts[contextId]->vm;
    retainVm(vm);
    if (result == 0 && !vm->ppgtt) {
        vm->ppgtt = gpu.createPpgtt();
        if (!vm->ppgtt) {
            result = err(kENOMEM);
        }
    }
    // A binding in the way (another object across the range, or this one
    // elsewhere) may be in use by requests still queued: wait until the
    // file's requests are done; and not more than kMaxQueued requests on the
    // GPU at once. Look again after each wait (others ran meanwhile).
    while (result == 0) {
        bool evict = false;
        for (Binding* binding = vm->bindings; binding && !evict; binding = binding->next) {
            for (uint32_t i = 0; i < held && !evict; ++i) {
                const uint64_t va = list[i].offset & (kVaLimit - 1);
                const bool same = binding->object == objects[i] && binding->va == va;
                const bool overlaps = va < binding->va + binding->size && binding->va < va + objects[i]->size;
                evict = !same && (overlaps || binding->object == objects[i]);
            }
        }
        if (evict && file->inflight != 0) {
            waitFor([&] { return file->inflight == 0; }, kForever);
            continue;
        }
        if (gpu.queuedRequests() >= kMaxQueued) {
            waitFor([&] { return gpu.queuedRequests() < kMaxQueued; }, kForever);
            continue;
        }
        break;
    }
    // From here on nothing sleeps. The context is looked up again: the
    // waits let other threads destroy it or change it.
    DrmContext* context = nullptr;
    HwContext* hw = nullptr;
    uint32_t engineIndex = 0;
    if (result == 0) {
        context = file->contexts[contextId];
        if (!context) {
            result = err(kENOENT);
        } else if (context->vm != vm || !pickEngine(context, ring, &engine)) {
            result = err(kEINVAL);
        } else if (context->banned) {
            result = err(kEIO);
        } else {
            engineIndex = engine == EngineId::Blitter ? 1 : 0;
            const uint32_t slot = context->engineMapCount != 0 ? static_cast<uint32_t>(ring) : engineIndex;
            hw = &context->engines[slot];
            if (!hw->image && !gpu.createHwContext(engine, *hw)) {
                result = err(kENOMEM);
            }
        }
    }
    // Bind: keep a binding already in place, drop ones in the way.
    for (uint32_t i = 0; result == 0 && i < held; ++i) {
        GemObject* object = objects[i];
        const uint64_t va = list[i].offset & (kVaLimit - 1);
        bool bound = false;
        Binding* binding = vm->bindings;
        while (binding) {
            Binding* next = binding->next;
            const bool same = binding->object == object && binding->va == va;
            const bool overlaps = va < binding->va + binding->size && binding->va < va + object->size;
            if (same) {
                bound = true;
            } else if (overlaps || binding->object == object) {
                unbind(vm, binding);
            }
            binding = next;
        }
        if (bound) {
            continue;
        }
        auto* fresh = new Binding{va, object->size, object, vm->bindings};
        const GpuBuffer pages = object->view();
        if (!fresh || !gpu.ppgttMap(vm->ppgtt, va, &pages, object->cached)) {
            delete fresh;
            gpu.ppgttUnmap(vm->ppgtt, va, object->size);
            result = err(kENOSPC);
            break;
        }
        object->retain();
        vm->bindings = fresh;
    }
    for (uint32_t i = 0; result == 0 && i < fenceCount; ++i) {
        Syncobj* sync = lookupSyncobj(file, fences[i].handle);
        if (!sync) {
            result = err(kENOENT);
        } else if (fences[i].flags & I915_EXEC_FENCE_WAIT) {
            Fence* fence = syncFenceFor(sync, pointOf(i));
            if (!fence) {
                result = err(kEINVAL);   // waiting on a sync object (or point) with no fence
            } else {
                fence->release();
            }
        }
    }
    GemRequest* g = nullptr;
    Fence* done = nullptr;
    Fence** deps = nullptr;
    if (result == 0) {
        g = new GemRequest();
        done = new Fence();
        deps = static_cast<Fence**>(kmalloc(sizeof(Fence*) * (3 * held + fenceCount + 2)));
        if (!g || !done || !deps) {
            delete g;
            delete done;
            if (deps) kfree(deps);
            result = err(kENOMEM);
        }
    }
    releaseVm(vm);   // from here the context's reference keeps it
    if (result != 0) {
        for (uint32_t i = 0; i < held; ++i) {
            objects[i]->release();
        }
        kfree(list);
        kfree(objects);
        if (fences) kfree(fences);
        if (points) kfree(points);
        if (inFence) inFence->release();
        return result;
    }

    // Objects the GPU does not snoop: out of the CPU caches first. Not a
    // scanout buffer's borrowed pages: the CPU writes those write-combining
    // only, so no line of theirs is ever in its caches, and clflushing a
    // screen's worth (8 MB) on each of the compositor's frames cost 27 ms.
    for (uint32_t i = 0; i < held; ++i) {
        if (!objects[i]->cached && !objects[i]->borrowed) {
            flushObject(objects[i]);
        }
    }
    // Dependencies, then this request's fence where later ones will look.
    uint32_t depCount = 0;
    for (uint32_t i = 0; i < held; ++i) {
        if (list[i].flags & EXEC_OBJECT_ASYNC) {
            continue;
        }
        addDependency(deps, depCount, objects[i]->write);
        if (list[i].flags & EXEC_OBJECT_WRITE) {
            addDependency(deps, depCount, objects[i]->reads[0]);
            addDependency(deps, depCount, objects[i]->reads[1]);
        }
    }
    for (uint32_t i = 0; i < fenceCount; ++i) {
        if (fences[i].flags & I915_EXEC_FENCE_WAIT) {
            Fence* fence = syncFenceFor(lookupSyncobj(file, fences[i].handle), pointOf(i));
            if (fence) {
                addDependency(deps, depCount, fence);
                fence->release();
            }
        }
    }
    if (inFence) {
        addDependency(deps, depCount, inFence);
        inFence->release();
    }
    if (context->singleTimeline) {
        addDependency(deps, depCount, context->timeline);
        setFence(context->timeline, done);
    }
    for (uint32_t i = 0; i < held; ++i) {
        setFence(objects[i]->reads[engineIndex], done);
        if (list[i].flags & EXEC_OBJECT_WRITE) {
            setFence(objects[i]->write, done);
            objects[i]->writeEngine = static_cast<uint8_t>(engineIndex);
        }
    }
    for (uint32_t i = 0; i < fenceCount; ++i) {
        if (fences[i].flags & I915_EXEC_FENCE_SIGNAL) {
            syncSignal(lookupSyncobj(file, fences[i].handle), pointOf(i), done);
        }
    }

    g->file = file;
    g->context = context;
    g->objects = objects;
    g->objectCount = held;
    Request& rq = g->rq;
    rq.engine = engine;
    rq.context = hw;
    rq.vm = vm->ppgtt;
    rq.batchVa = (list[batchIndex].offset & (kVaLimit - 1)) + exec.batch_start_offset;
    rq.done = done;
    rq.deps = depCount ? deps : nullptr;
    rq.depCount = depCount;
    if (!depCount) {
        kfree(deps);
    }
    rq.skip = &context->banned;
    rq.retire = retireGemRequest;
    rq.owner = g;
    ++file->inflight;
    ++context->pending;
    context->used = true;
    kfree(list);
    if (fences) kfree(fences);
    if (points) kfree(points);
    done->retain();   // for FENCE_OUT, past the request's retirement
    const int queued = gpu.queueRequest(&rq);
    if (queued != 0) {
        done->signaled = true;
        done->status = queued;
        retireGemRequest(&rq);
        done->release();
        return err(-queued);
    }
    if (fenceOut) {
        // The batch's fence as a sync_file, in rsvd2's upper half.
        const int64_t slot = exportSyncFile(done, true);
        done->release();
        if (slot < 0) {
            return err(-slot);
        }
        const uint64_t rsvd2 = (exec.rsvd2 & 0xFFFFFFFFull) | (static_cast<uint64_t>(slot) << 32);
        if (!Syscall::copyToUser(arg + __builtin_offsetof(drm_i915_gem_execbuffer2, rsvd2), &rsvd2, sizeof(rsvd2))) {
            return err(kEFAULT);
        }
        return 0;
    }
    done->release();
    return 0;
}

uint64_t doGemWait(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_wait request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    GemObject* object = lookup(file, request.bo_handle);
    if (!object || request.flags != 0) {
        return err(object ? kEINVAL : kENOENT);
    }
    // timeout_ns: below 0 forever, 0 just a check; what is left goes back.
    const uint64_t start = time_get_uptime_ns();
    const uint64_t deadline = request.timeout_ns < 0 ? kForever : start + static_cast<uint64_t>(request.timeout_ns);
    object->retain();
    const bool idle = waitFor([&] { return objectIdle(object); }, deadline);
    object->release();
    if (request.timeout_ns > 0) {
        const uint64_t spent = time_get_uptime_ns() - start;
        request.timeout_ns = spent >= static_cast<uint64_t>(request.timeout_ns) ? 0 : request.timeout_ns - static_cast<int64_t>(spent);
        copyOut(arg, &request, size);
    }
    return idle ? 0 : err(kETIME);
}

// GEM_BUSY as i915 reports it: the engine class writing it, plus one, in
// the low half; a bit per engine class reading it in the high half.
uint64_t doGemBusy(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_busy request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    GemObject* object = lookup(file, request.handle);
    if (!object) {
        return err(kENOENT);
    }
    if (gpuPresent()) {
        IntelGen9Driver::get().processRequests();
    }
    uint32_t busy = 0;
    for (uint32_t engine = 0; engine < 2; ++engine) {
        if (fenceBusy(object->reads[engine])) {
            busy |= 1u << (16 + engine);   // render is class 0, copy class 1
        }
    }
    if (fenceBusy(object->write)) {
        busy |= object->writeEngine + 1u;
    }
    request.busy = busy;
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// SET_DOMAIN: the CPU is about to read (wait for the GPU's writes) or write
// (wait for every GPU use). The LLC keeps the caches coherent otherwise.
uint64_t doSetDomain(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_set_domain request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    GemObject* object = lookup(file, request.handle);
    if (!object) {
        return err(kENOENT);
    }
    object->retain();
    if (request.write_domain != 0) {
        waitFor([&] { return objectIdle(object); }, kForever);
    } else {
        waitFor([&] { return !fenceBusy(object->write); }, kForever);
    }
    if (!object->cached) {
        flushObject(object);   // the GPU wrote memory, not the caches (i915: clflush for CPU access)
    }
    object->release();
    return 0;
}

uint64_t doGetCap(uint64_t arg, uint32_t size) {
    drm_get_cap request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    switch (request.capability) {
        case DRM_CAP_SYNCOBJ:
        case DRM_CAP_SYNCOBJ_TIMELINE:
        case DRM_CAP_TIMESTAMP_MONOTONIC:
            request.value = 1;
            break;
        case DRM_CAP_PRIME:
            request.value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
            break;
        default:
            return err(kEINVAL);
    }
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// A new handle in the file for `sync` (the reference passes to it); 0 if out of memory.
uint32_t addSyncHandle(DrmFile* file, Syncobj* sync) {
    uint32_t index = 0;
    while (index < file->syncobjCapacity && file->syncobjs[index]) {
        ++index;
    }
    if (index == file->syncobjCapacity) {
        const uint32_t capacity = file->syncobjCapacity ? file->syncobjCapacity * 2 : 16;
        auto* grown = static_cast<Syncobj**>(kmalloc(sizeof(Syncobj*) * capacity));
        if (!grown) {
            return 0;
        }
        for (uint32_t i = 0; i < capacity; ++i) {
            grown[i] = i < file->syncobjCapacity ? file->syncobjs[i] : nullptr;
        }
        if (file->syncobjs) {
            kfree(file->syncobjs);
        }
        file->syncobjs = grown;
        file->syncobjCapacity = capacity;
    }
    file->syncobjs[index] = sync;
    return index + 1;
}

uint64_t doSyncobjCreate(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_syncobj_create request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if ((request.flags & ~DRM_SYNCOBJ_CREATE_SIGNALED) != 0) {
        return err(kEINVAL);
    }
    auto* sync = new Syncobj();
    if (!sync) {
        return err(kENOMEM);
    }
    if (request.flags & DRM_SYNCOBJ_CREATE_SIGNALED) {
        sync->fence = signaledFence();
        if (!sync->fence) {
            releaseSync(sync);
            return err(kENOMEM);
        }
    }
    request.handle = addSyncHandle(file, sync);
    if (request.handle == 0) {
        releaseSync(sync);
        return err(kENOMEM);
    }
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

uint64_t doSyncobjDestroy(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_syncobj_destroy request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    Syncobj* sync = lookupSyncobj(file, request.handle);
    if (!sync || request.pad != 0) {
        return err(kEINVAL);
    }
    file->syncobjs[request.handle - 1] = nullptr;
    releaseSync(sync);
    return 0;
}

// The handles (and, for timeline calls, the points) of a sync object call,
// copied in; nullptr on a bad count or address (the error in *error).
uint32_t* copyHandles(uint64_t user, uint32_t count, uint64_t* error) {
    if (count == 0 || count > kMaxExecFences) {
        *error = err(kEINVAL);
        return nullptr;
    }
    auto* handles = static_cast<uint32_t*>(kmalloc(sizeof(uint32_t) * count));
    if (!handles || !Syscall::copyFromUser(handles, user, sizeof(uint32_t) * count)) {
        if (handles) kfree(handles);
        *error = err(handles ? kEFAULT : kENOMEM);
        return nullptr;
    }
    return handles;
}

uint64_t* copyPoints(uint64_t user, uint32_t count, uint64_t* error) {
    auto* points = static_cast<uint64_t*>(kmalloc(sizeof(uint64_t) * count));
    if (!points || (user != 0 && !Syscall::copyFromUser(points, user, sizeof(uint64_t) * count))) {
        if (points) kfree(points);
        *error = err(points ? kEFAULT : kENOMEM);
        return nullptr;
    }
    if (user == 0) {
        memset(points, 0, sizeof(uint64_t) * count);
    }
    return points;
}

// SYNCOBJ_WAIT and SYNCOBJ_TIMELINE_WAIT (drm_syncobj_array_wait): until
// all (WAIT_ALL) or any of the points (0: the payload) are signaled, or
// with WAIT_AVAILABLE submitted, or the absolute CLOCK_MONOTONIC deadline.
// A point not yet submitted is an error unless WAIT_FOR_SUBMIT, which
// waits for it too. The sync objects are held for the wait.
uint64_t waitSyncobjs(DrmFile* file, const uint32_t* handles, const uint64_t* points, uint32_t count, uint32_t flags,
                      int64_t timeoutNs, uint32_t* first) {
    const bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
    const bool forSubmit = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
    const bool available = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
    auto* syncs = static_cast<Syncobj**>(kmalloc(sizeof(Syncobj*) * count));
    if (!syncs) {
        return err(kENOMEM);
    }
    uint64_t error = 0;
    uint32_t held = 0;
    for (uint32_t i = 0; i < count && error == 0; ++i) {
        Syncobj* sync = lookupSyncobj(file, handles[i]);
        if (!sync) {
            error = err(kENOENT);
            break;
        }
        Fence* fence = syncFenceFor(sync, points[i]);
        if (!fence && !forSubmit) {
            error = err(kEINVAL);
        }
        if (fence) {
            fence->release();
        }
        retainSync(sync);
        syncs[held++] = sync;
    }
    bool done = false;
    if (error == 0) {
        auto ready = [&] {
            uint32_t reached = 0;
            for (uint32_t i = 0; i < count; ++i) {
                Fence* fence = syncFenceFor(syncs[i], points[i]);
                const bool ok = fence && (available || fence->poll());
                if (fence) {
                    fence->release();
                }
                if (ok) {
                    if (reached++ == 0) {
                        *first = i;
                    }
                    if (!all) {
                        return true;
                    }
                }
            }
            return reached == count;
        };
        const uint64_t deadline = timeoutNs <= 0 ? 0 : static_cast<uint64_t>(timeoutNs);
        done = waitFor(ready, deadline);
    }
    for (uint32_t i = 0; i < held; ++i) {
        releaseSync(syncs[i]);
    }
    kfree(syncs);
    if (error != 0) {
        return error;
    }
    return done ? 0 : err(kETIME);
}

uint64_t doSyncobjWait(DrmFile* file, uint64_t arg, uint32_t size, bool timeline) {
    uint64_t handlesPtr = 0, pointsPtr = 0;
    int64_t timeout = 0;
    uint32_t count = 0, flags = 0;
    drm_syncobj_wait binary;
    drm_syncobj_timeline_wait points;
    if (timeline) {
        if (!copyIn(&points, arg, size)) return err(kEFAULT);
        handlesPtr = points.handles;
        pointsPtr = points.points;
        timeout = points.timeout_nsec;
        count = points.count_handles;
        flags = points.flags;
    } else {
        if (!copyIn(&binary, arg, size)) return err(kEFAULT);
        handlesPtr = binary.handles;
        timeout = binary.timeout_nsec;
        count = binary.count_handles;
        flags = binary.flags;
    }
    uint32_t known = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
    if (timeline) {
        known |= DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
    }
    if ((flags & ~known) != 0) {
        return err(kEINVAL);
    }
    uint64_t error = 0;
    uint32_t* handles = copyHandles(handlesPtr, count, &error);
    if (!handles) {
        return error;
    }
    uint64_t* values = copyPoints(timeline ? pointsPtr : 0, count, &error);
    if (!values) {
        kfree(handles);
        return error;
    }
    uint32_t first = 0;
    error = waitSyncobjs(file, handles, values, count, flags, timeout, &first);
    kfree(handles);
    kfree(values);
    if (error != 0) {
        return error;
    }
    if (timeline) {
        points.first_signaled = first;
        return copyOut(arg, &points, size) ? 0 : err(kEFAULT);
    }
    binary.first_signaled = first;
    return copyOut(arg, &binary, size) ? 0 : err(kEFAULT);
}

// SYNCOBJ_RESET (no fence) and SIGNAL (a signaled one), and
// SYNCOBJ_TIMELINE_SIGNAL (a signaled point; point 0 the payload).
uint64_t doSyncobjSignal(DrmFile* file, uint64_t arg, uint32_t size, int mode) {
    uint64_t handlesPtr = 0, pointsPtr = 0;
    uint32_t count = 0;
    if (mode == 2) {
        drm_syncobj_timeline_array request;
        if (!copyIn(&request, arg, size)) return err(kEFAULT);
        if (request.flags != 0) return err(kEINVAL);
        handlesPtr = request.handles;
        pointsPtr = request.points;
        count = request.count_handles;
    } else {
        drm_syncobj_array request;
        if (!copyIn(&request, arg, size)) return err(kEFAULT);
        if (request.pad != 0) return err(kEINVAL);
        handlesPtr = request.handles;
        count = request.count_handles;
    }
    uint64_t error = 0;
    uint32_t* handles = copyHandles(handlesPtr, count, &error);
    if (!handles) {
        return error;
    }
    uint64_t* points = copyPoints(pointsPtr, count, &error);
    if (!points) {
        kfree(handles);
        return error;
    }
    for (uint32_t i = 0; i < count && error == 0; ++i) {
        if (!lookupSyncobj(file, handles[i])) {
            error = err(kENOENT);
        }
    }
    for (uint32_t i = 0; i < count && error == 0; ++i) {
        Syncobj* sync = lookupSyncobj(file, handles[i]);
        if (mode == 0) {
            syncSetPayload(sync, nullptr);
            continue;
        }
        Fence* fence = signaledFence();
        if (!fence || !syncSignal(sync, points[i], fence)) {
            error = err(kENOMEM);
        }
        if (fence) {
            fence->release();   // the sync object holds it now
        }
    }
    kfree(handles);
    kfree(points);
    return error;
}

// SYNCOBJ_QUERY: each timeline's highest point reached (or, with
// LAST_SUBMITTED, added); 0 for a binary one.
uint64_t doSyncobjQuery(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_syncobj_timeline_array request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if ((request.flags & ~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) != 0) {
        return err(kEINVAL);
    }
    uint64_t error = 0;
    uint32_t* handles = copyHandles(request.handles, request.count_handles, &error);
    if (!handles) {
        return error;
    }
    for (uint32_t i = 0; i < request.count_handles && error == 0; ++i) {
        Syncobj* sync = lookupSyncobj(file, handles[i]);
        if (!sync) {
            error = err(kENOENT);
            break;
        }
        const uint64_t value = (request.flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) ? sync->lastPoint : syncQuery(sync);
        if (!Syscall::copyToUser(request.points + i * sizeof(uint64_t), &value, sizeof(value))) {
            error = err(kEFAULT);
        }
    }
    kfree(handles);
    return error;
}

// SYNCOBJ_TRANSFER: the fence for a point (or the payload) of one sync
// object into a point (or the payload) of another.
uint64_t doSyncobjTransfer(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_syncobj_transfer request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if (request.pad != 0 || (request.flags & ~DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0) {
        return err(kEINVAL);
    }
    Syncobj* source = lookupSyncobj(file, request.src_handle);
    Syncobj* target = lookupSyncobj(file, request.dst_handle);
    if (!source || !target) {
        return err(kENOENT);
    }
    Fence* fence = syncFenceFor(source, request.src_point);
    if (!fence) {
        return err(kEINVAL);   // that point is not submitted yet
    }
    const bool ok = syncSignal(target, request.dst_point, fence);
    fence->release();
    return ok ? 0 : err(kENOMEM);
}

// SYNCOBJ_HANDLE_TO_FD: the sync object itself as an fd, or (EXPORT_SYNC_FILE)
// a sync_file of its fence for the payload or a point.
uint64_t doSyncobjToFd(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_syncobj_handle request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    const uint32_t known = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE | DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE;
    const bool exportFile = request.flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    const bool timeline = request.flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE;
    if ((request.flags & ~known) != 0 || request.pad != 0 || (timeline && !exportFile) ||
        (!timeline && request.point != 0)) {
        return err(kEINVAL);
    }
    Syncobj* sync = lookupSyncobj(file, request.handle);
    if (!sync) {
        return err(kENOENT);
    }
    int64_t slot = 0;
    if (exportFile) {
        Fence* fence = syncFenceFor(sync, timeline ? request.point : 0);
        if (!fence) {
            return err(kEINVAL);   // nothing to export (Linux too)
        }
        slot = exportSyncFile(fence, true);
        fence->release();
    } else {
        retainSync(sync);
        VNode* node = makeNode(&gSyncobjFdOps, sync, FileType::CharDevice);
        if (!node) {
            releaseSync(sync);
            return err(kENOMEM);
        }
        slot = installNode(node, true);
    }
    if (slot < 0) {
        return err(-slot);
    }
    request.fd = static_cast<int32_t>(slot);
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// SYNCOBJ_FD_TO_HANDLE: a sync object fd into a handle of this file, or
// (IMPORT_SYNC_FILE) a sync_file's fence into an existing sync object's
// payload or a point.
uint64_t doSyncobjFromFd(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_syncobj_handle request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    const uint32_t known = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE | DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE;
    const bool importFile = request.flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    const bool timeline = request.flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE;
    if ((request.flags & ~known) != 0 || request.pad != 0 || (timeline && !importFile) ||
        (!timeline && request.point != 0)) {
        return err(kEINVAL);
    }
    if (importFile) {
        Fence* fence = syncFileFence(request.fd);
        Syncobj* sync = lookupSyncobj(file, request.handle);
        if (!fence) {
            return err(kEINVAL);
        }
        if (!sync) {
            return err(kENOENT);
        }
        return syncSignal(sync, timeline ? request.point : 0, fence) ? 0 : err(kENOMEM);
    }
    VNode* node = nodeOfFd(request.fd, &gSyncobjFdOps);
    if (!node) {
        return err(kEINVAL);
    }
    auto* sync = static_cast<Syncobj*>(node->getData());
    retainSync(sync);
    request.handle = addSyncHandle(file, sync);
    if (request.handle == 0) {
        releaseSync(sync);
        return err(kENOMEM);
    }
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// PRIME_HANDLE_TO_FD (drm_gem_prime_handle_to_fd): the object as a dma-buf
// fd, which any process holding it can import, map and size.
uint64_t doPrimeToFd(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_prime_handle request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if ((request.flags & ~(DRM_CLOEXEC | DRM_RDWR)) != 0) {
        return err(kEINVAL);
    }
    GemObject* object = lookup(file, request.handle);
    if (!object) {
        return err(kENOENT);
    }
    object->retain();
    VNode* node = makeNode(&gDmaBufOps, object, FileType::Regular);
    if (!node) {
        object->release();
        return err(kENOMEM);
    }
    const int64_t slot = installNode(node, request.flags & DRM_CLOEXEC);
    if (slot < 0) {
        return err(-slot);
    }
    request.fd = static_cast<int32_t>(slot);
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// Step 8b: pages the display scans out (a scanout buffer, which its driver
// keeps for good) as a dma-buf of the calling process, so the compositor can
// render into it on the GPU and then flip to it. Uncached: the display engine
// reads memory without snooping the caches, so the GPU's writes must go past
// the LLC (an uncached PPGTT mapping; Mesa draws imported buffers with the
// PTE's caching). The handle, or -errno.
int64_t exportPagesAsDmaBuf(const uint64_t* frames, uint32_t count) {
    GemObject* object = GemObject::borrow(frames, count);
    if (!object) {
        return -kENOMEM;
    }
    object->cached = false;
    VNode* node = makeNode(&gDmaBufOps, object, FileType::Regular);
    if (!node) {
        object->release();
        return -kENOMEM;
    }
    const int64_t slot = installNode(node, false);
    if (slot < 0) {
        return slot;
    }
    return static_cast<int64_t>(HandleTable::encodeHandle(HandleType::File, static_cast<uint32_t>(slot)));
}

// PRIME_FD_TO_HANDLE: a dma-buf fd into a handle; an object the file
// already has keeps its handle (drm_prime's lookup).
uint64_t doPrimeFromFd(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_prime_handle request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    VNode* node = nodeOfFd(request.fd, &gDmaBufOps);
    if (!node) {
        return err(kEINVAL);
    }
    auto* object = static_cast<GemObject*>(node->getData());
    for (uint32_t i = 0; i < file->handleCapacity; ++i) {
        if (file->handles[i] == object) {
            request.handle = i + 1;
            return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
        }
    }
    object->retain();
    request.handle = addHandle(file, object);
    if (request.handle == 0) {
        object->release();
        return err(kENOMEM);
    }
    if (!copyOut(arg, &request, size)) {
        file->handles[request.handle - 1] = nullptr;
        object->release();
        return err(kEFAULT);
    }
    return 0;
}

// sync_file ioctls (sync_file.c): MERGE (a new sync_file of both fences) and
// FILE_INFO (its state, and its fences').
uint64_t doSyncFileIoctl(VNode* node, uint64_t request, uint64_t arg) {
    auto* fence = static_cast<Fence*>(node->getData());
    const uint32_t size = iocSize(request);
    if (iocType(request) != kSyncIoctlType || !fence) {
        return err(kENOTTY);
    }
    if (iocNr(request) == SYNC_IOC_MERGE_NR) {
        sync_merge_data merge;
        if (!copyIn(&merge, arg, size)) {
            return err(kEFAULT);
        }
        if (merge.flags != 0 || merge.pad != 0) {
            return err(kEINVAL);
        }
        Fence* other = syncFileFence(merge.fd2);
        if (!other) {
            return err(kENOENT);
        }
        Fence* both[2] = {fence, other};
        Fence* joined = joinFences(both, 2);
        if (!joined) {
            return err(kENOMEM);
        }
        const int64_t slot = exportSyncFile(joined, true);
        joined->release();
        if (slot < 0) {
            return err(-slot);
        }
        merge.fence = static_cast<int32_t>(slot);
        return copyOut(arg, &merge, size) ? 0 : err(kEFAULT);
    }
    if (iocNr(request) == SYNC_IOC_FILE_INFO_NR) {
        sync_file_info info;
        if (!copyIn(&info, arg, size)) {
            return err(kEFAULT);
        }
        if (info.flags != 0 || info.pad != 0) {
            return err(kEINVAL);
        }
        auto statusOf = [](Fence* f) { return f->poll() ? (f->status < 0 ? f->status : 1) : 0; };
        const uint32_t count = fence->parts ? fence->partCount : 1;
        memset(info.name, 0, sizeof(info.name));
        memcpy(info.name, "i915", 4);
        info.status = statusOf(fence);
        if (info.num_fences != 0) {
            if (info.num_fences < count) {
                return err(kEINVAL);
            }
            for (uint32_t i = 0; i < count; ++i) {
                Fence* part = fence->parts ? fence->parts[i] : fence;
                sync_fence_info one = {};
                memcpy(one.obj_name, "request", 7);
                memcpy(one.driver_name, "i915", 4);
                one.status = statusOf(part);
                if (!Syscall::copyToUser(info.sync_fence_info + i * sizeof(one), &one, sizeof(one))) {
                    return err(kEFAULT);
                }
            }
        }
        info.num_fences = count;
        return copyOut(arg, &info, size) ? 0 : err(kEFAULT);
    }
    return err(kENOTTY);
}

// The fences a dma-buf's users must respect (dma_resv): for a reader the
// writer, for a writer everyone. A new reference (signaled if none).
Fence* objectFences(GemObject* object, bool forWrite) {
    Fence* list[3] = {object->write, forWrite ? object->reads[0] : nullptr, forWrite ? object->reads[1] : nullptr};
    return joinFences(list, 3);
}

// dma-buf ioctls (dma-buf.c): SYNC around CPU access (waits for the GPU; an
// uncached object leaves the CPU caches), EXPORT_SYNC_FILE / IMPORT_SYNC_FILE
// for implicit synchronisation with fences from elsewhere, SET_NAME ignored.
uint64_t doDmaBufIoctl(VNode* node, uint64_t request, uint64_t arg) {
    auto* object = static_cast<GemObject*>(node->getData());
    const uint32_t size = iocSize(request);
    if (iocType(request) != kDmaBufIoctlType || !object) {
        return err(kENOTTY);
    }
    switch (iocNr(request)) {
        case DMA_BUF_IOCTL_SYNC_NR: {
            dma_buf_sync sync;
            if (!copyIn(&sync, arg, size)) {
                return err(kEFAULT);
            }
            if ((sync.flags & ~(DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END)) != 0 || (sync.flags & DMA_BUF_SYNC_RW) == 0) {
                return err(kEINVAL);
            }
            if (sync.flags & DMA_BUF_SYNC_END) {
                if (!object->cached && (sync.flags & DMA_BUF_SYNC_WRITE)) {
                    flushObject(object);   // the CPU's writes, out to memory for the GPU
                }
                return 0;
            }
            const bool forWrite = sync.flags & DMA_BUF_SYNC_WRITE;
            object->retain();
            waitFor([&] { return forWrite ? objectIdle(object) : !fenceBusy(object->write); }, kForever);
            if (!object->cached) {
                flushObject(object);
            }
            object->release();
            return 0;
        }
        case DMA_BUF_SET_NAME_NR:
            return 0;
        case DMA_BUF_IOCTL_EXPORT_SYNC_FILE_NR: {
            dma_buf_export_sync_file exported;
            if (!copyIn(&exported, arg, size)) {
                return err(kEFAULT);
            }
            if (exported.flags == 0 || (exported.flags & ~DMA_BUF_SYNC_RW) != 0) {
                return err(kEINVAL);
            }
            Fence* fence = objectFences(object, exported.flags & DMA_BUF_SYNC_WRITE);
            if (!fence) {
                return err(kENOMEM);
            }
            const int64_t slot = exportSyncFile(fence, true);
            fence->release();
            if (slot < 0) {
                return err(-slot);
            }
            exported.fd = static_cast<int32_t>(slot);
            return copyOut(arg, &exported, size) ? 0 : err(kEFAULT);
        }
        case DMA_BUF_IOCTL_IMPORT_SYNC_FILE_NR: {
            dma_buf_import_sync_file imported;
            if (!copyIn(&imported, arg, size)) {
                return err(kEFAULT);
            }
            if (imported.flags == 0 || (imported.flags & ~DMA_BUF_SYNC_RW) != 0) {
                return err(kEINVAL);
            }
            Fence* fence = syncFileFence(imported.fd);
            if (!fence) {
                return err(kEINVAL);
            }
            // As a writer: later users wait for it; as a reader, later writers do.
            Fence* list[2] = {(imported.flags & DMA_BUF_SYNC_WRITE) ? object->write : object->reads[0], fence};
            Fence* joined = joinFences(list, 2);
            if (!joined) {
                return err(kENOMEM);
            }
            setFence((imported.flags & DMA_BUF_SYNC_WRITE) ? object->write : object->reads[0], joined);
            joined->release();
            return 0;
        }
        default:
            return err(kENOTTY);
    }
}

// QUERY (i915_query.c): topology, engines and memory regions; i915 reports
// what a part lacks (hwconfig, geometry subslices, GuC, perf) as -ENODEV
// in the item's length.
int32_t queryTopology(const drm_i915_query_item& item) {
    if (item.flags != 0) {
        return -kEINVAL;
    }
    const intel_gen9::Gen9Sseu* sseu = gpuPresent() ? IntelGen9Driver::get().topology() : nullptr;
    if (!sseu) {
        return -kENODEV;
    }
    uint8_t data[intel_gen9::kGen9TopologyBytes];
    const uint32_t bytes = intel_gen9::encodeGen9Topology(*sseu, data, sizeof(data));
    if (item.length == 0) {
        return static_cast<int32_t>(bytes);
    }
    if (item.length < static_cast<int32_t>(bytes)) {
        return -kEINVAL;
    }
    return Syscall::copyToUser(item.data_ptr, data, bytes) ? static_cast<int32_t>(bytes) : -kEFAULT;
}

// Engine info and memory regions: the caller's header must be zero (i915).
template <typename Header>
int32_t checkQueryHeader(const drm_i915_query_item& item, uint32_t bytes) {
    if (item.flags != 0) {
        return -kEINVAL;
    }
    if (item.length == 0) {
        return static_cast<int32_t>(bytes);
    }
    if (item.length < static_cast<int32_t>(bytes)) {
        return -kEINVAL;
    }
    Header header;
    if (!Syscall::copyFromUser(&header, item.data_ptr, sizeof(header))) {
        return -kEFAULT;
    }
    const auto* raw = reinterpret_cast<const uint8_t*>(&header);
    for (uint32_t i = 0; i < sizeof(header); ++i) {
        if (raw[i] != 0) {
            return -kEINVAL;
        }
    }
    return 0;
}

int32_t queryEngines(const drm_i915_query_item& item) {
    constexpr uint32_t kEngines = 2;
    const uint32_t bytes = sizeof(drm_i915_query_engine_info) + kEngines * sizeof(drm_i915_engine_info);
    const int32_t checked = checkQueryHeader<drm_i915_query_engine_info>(item, bytes);
    if (checked != 0) {
        return checked;
    }
    uint8_t data[sizeof(drm_i915_query_engine_info) + kEngines * sizeof(drm_i915_engine_info)] = {};
    auto* header = reinterpret_cast<drm_i915_query_engine_info*>(data);
    auto* engines = reinterpret_cast<drm_i915_engine_info*>(data + sizeof(*header));
    header->num_engines = kEngines;
    const uint16_t classes[kEngines] = {I915_ENGINE_CLASS_RENDER, I915_ENGINE_CLASS_COPY};
    for (uint32_t i = 0; i < kEngines; ++i) {
        engines[i].engine.engine_class = classes[i];
        engines[i].engine.engine_instance = 0;
        engines[i].flags = I915_ENGINE_INFO_HAS_LOGICAL_INSTANCE;
        engines[i].logical_instance = 0;
    }
    return Syscall::copyToUser(item.data_ptr, data, bytes) ? static_cast<int32_t>(bytes) : -kEFAULT;
}

int32_t queryMemoryRegions(const drm_i915_query_item& item) {
    const uint32_t bytes = sizeof(drm_i915_query_memory_regions) + sizeof(drm_i915_memory_region_info);
    const int32_t checked = checkQueryHeader<drm_i915_query_memory_regions>(item, bytes);
    if (checked != 0) {
        return checked;
    }
    uint8_t data[sizeof(drm_i915_query_memory_regions) + sizeof(drm_i915_memory_region_info)] = {};
    auto* header = reinterpret_cast<drm_i915_query_memory_regions*>(data);
    auto* region = reinterpret_cast<drm_i915_memory_region_info*>(data + sizeof(*header));
    header->num_regions = 1;   // an integrated part: system memory only
    region->region.memory_class = I915_MEMORY_CLASS_SYSTEM;
    region->region.memory_instance = 0;
    // Usable RAM (TotalMemory counts up to the highest frame, holes and all).
    region->probed_size = PMM::UsableMemory();
    region->unallocated_size = PMM::UsableMemory() - PMM::UsedUsableMemory();
    region->probed_cpu_visible_size = region->probed_size;
    region->unallocated_cpu_visible_size = region->unallocated_size;
    return Syscall::copyToUser(item.data_ptr, data, bytes) ? static_cast<int32_t>(bytes) : -kEFAULT;
}

uint64_t doQuery(uint64_t arg, uint32_t size) {
    drm_i915_query request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if (request.flags != 0) {
        return err(kEINVAL);
    }
    for (uint32_t i = 0; i < request.num_items; ++i) {
        const uint64_t at = request.items_ptr + static_cast<uint64_t>(i) * sizeof(drm_i915_query_item);
        drm_i915_query_item item;
        if (!Syscall::copyFromUser(&item, at, sizeof(item))) {
            return err(kEFAULT);
        }
        int32_t length = -kEINVAL;
        switch (item.query_id) {
            case DRM_I915_QUERY_TOPOLOGY_INFO:
                length = queryTopology(item);
                break;
            case DRM_I915_QUERY_ENGINE_INFO:
                length = queryEngines(item);
                break;
            case DRM_I915_QUERY_MEMORY_REGIONS:
                length = queryMemoryRegions(item);
                break;
            case DRM_I915_QUERY_PERF_CONFIG:
            case DRM_I915_QUERY_HWCONFIG_BLOB:
            case DRM_I915_QUERY_GEOMETRY_SUBSLICES:
            case DRM_I915_QUERY_GUC_SUBMISSION_VERSION:
                length = -kENODEV;
                break;
            default:
                break;
        }
        if (!Syscall::copyToUser(at + __builtin_offsetof(drm_i915_query_item, length), &length, sizeof(length))) {
            return err(kEFAULT);
        }
    }
    return 0;
}

// REG_READ (i915_reg_read_ioctl): only the render engine's timestamp, as
// i915's whitelist; I915_REG_READ_8B_WA in the low bits reads it as two
// dwords (this always does).
uint64_t doRegRead(uint64_t arg, uint32_t size) {
    drm_i915_reg_read request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    constexpr uint64_t kRenderTimestamp = 0x2358;
    if ((request.offset & ~7ull) != kRenderTimestamp || (request.offset & 7ull & ~I915_REG_READ_8B_WA) != 0) {
        return err(kEINVAL);
    }
    if (!gpuPresent() || !IntelGen9Driver::get().readRenderTimestamp(&request.val)) {
        return err(kENODEV);
    }
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// GET_APERTURE: the GGTT's size and what of it is free (i915 reports its
// whole GGTT).
uint64_t doGetAperture(uint64_t arg, uint32_t size) {
    drm_i915_gem_get_aperture request = {};
    IntelGen9Driver& gpu = IntelGen9Driver::get();
    request.aper_size = gpu.gttTotalBytes();
    request.aper_available_size = gpu.gttFreeBytes();
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// SET_TILING / GET_TILING (i915_gem_tiling.c): recorded for the object's
// users (with full PPGTT, nothing in the kernel detiles); i915_tiling_ok's
// checks for Gen7+ (stride at most 256 KiB, a multiple of the tile width:
// 512 bytes for X, 128 for Y). Gen9 has no bit-6 swizzling.
uint64_t doSetTiling(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_set_tiling request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    GemObject* object = lookup(file, request.handle);
    if (!object) {
        return err(kENOENT);
    }
    if (request.tiling_mode > I915_TILING_Y) {
        return err(kEINVAL);
    }
    if (request.tiling_mode != I915_TILING_NONE) {
        const uint32_t tileWidth = request.tiling_mode == I915_TILING_Y ? 128 : 512;
        if (request.stride == 0 || request.stride % tileWidth != 0 || request.stride / 128 > 0x800) {
            return err(kEINVAL);
        }
    } else {
        request.stride = 0;
    }
    object->tiling = request.tiling_mode;
    object->stride = request.stride;
    request.swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

uint64_t doGetTiling(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_get_tiling request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    GemObject* object = lookup(file, request.handle);
    if (!object) {
        return err(kENOENT);
    }
    request.tiling_mode = object->tiling;
    request.swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
    request.phys_swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// SET_CACHING / GET_CACHING (i915_gem_domain.c): CACHED maps the object
// LLC-cached in the PPGTT, NONE and DISPLAY (no write-through on this part)
// uncached. A change waits for the GPU and drops the object's bindings, so
// the next execbuffer maps it the new way.
uint64_t doSetCaching(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_caching request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if (request.caching != I915_CACHING_NONE && request.caching != I915_CACHING_CACHED &&
        request.caching != I915_CACHING_DISPLAY) {
        return err(kEINVAL);
    }
    GemObject* object = lookup(file, request.handle);
    if (!object) {
        return err(kENOENT);
    }
    const bool cached = request.caching == I915_CACHING_CACHED;
    if (cached == object->cached) {
        return 0;
    }
    object->retain();
    waitFor([&] { return objectIdle(object); }, kForever);
    if (lookup(file, request.handle) != object) {
        object->release();
        return err(kENOENT);   // closed meanwhile
    }
    unbindObject(file, object);
    object->cached = cached;
    if (!cached) {
        flushObject(object);
    }
    object->release();
    return 0;
}

uint64_t doGetCaching(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_caching request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    GemObject* object = lookup(file, request.handle);
    if (!object) {
        return err(kENOENT);
    }
    request.caching = object->cached ? I915_CACHING_CACHED : I915_CACHING_NONE;
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// MADVISE: recorded; nothing is purged under memory pressure yet, so the
// pages are always retained.
uint64_t doMadvise(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_madvise request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if (request.madv != I915_MADV_WILLNEED && request.madv != I915_MADV_DONTNEED) {
        return err(kEINVAL);
    }
    GemObject* object = lookup(file, request.handle);
    if (!object) {
        return err(kENOENT);
    }
    object->madv = request.madv;
    request.retained = 1;
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

// GEM_CREATE_EXT (i915_gem_create.c): GEM_CREATE with extensions. The only
// memory region is system memory (class 0, instance 0); protected content
// and SET_PAT are not on this part (ENODEV, as i915 before Meteor Lake).
uint64_t doCreateExt(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_gem_create_ext request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if (request.flags & ~I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS) {
        return err(kEINVAL);
    }
    if (request.flags & I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS) {
        return err(kEINVAL);   // only for device-memory placements
    }
    bool sawRegions = false;
    uint64_t next = request.extensions;
    for (uint32_t links = 0; next != 0; ++links) {
        i915_user_extension base;
        if (links == 16) {
            return err(kEINVAL);
        }
        if (!Syscall::copyFromUser(&base, next, sizeof(base))) {
            return err(kEFAULT);
        }
        if (base.flags != 0 || base.rsvd[0] || base.rsvd[1] || base.rsvd[2] || base.rsvd[3]) {
            return err(kEINVAL);
        }
        if (base.name == I915_GEM_CREATE_EXT_MEMORY_REGIONS) {
            drm_i915_gem_create_ext_memory_regions regions;
            if (sawRegions) {
                return err(kEINVAL);   // given twice
            }
            sawRegions = true;
            if (!Syscall::copyFromUser(&regions, next, sizeof(regions))) {
                return err(kEFAULT);
            }
            if (regions.pad != 0 || regions.num_regions != 1) {
                return err(kEINVAL);   // one region exists, so one placement
            }
            drm_i915_gem_memory_class_instance placement;
            if (!Syscall::copyFromUser(&placement, regions.regions, sizeof(placement))) {
                return err(kEFAULT);
            }
            if (placement.memory_class != I915_MEMORY_CLASS_SYSTEM || placement.memory_instance != 0) {
                return err(kEINVAL);
            }
        } else if (base.name == I915_GEM_CREATE_EXT_PROTECTED_CONTENT || base.name == I915_GEM_CREATE_EXT_SET_PAT) {
            return err(kENODEV);
        } else {
            return err(kEINVAL);
        }
        next = base.next_extension;
    }
    const uint64_t bytes = (request.size + kPage - 1) & ~(kPage - 1);
    if (bytes == 0 || bytes > kMaxObjectBytes) {
        return err(kEINVAL);
    }
    GemObject* object = GemObject::create(bytes);
    if (!object) {
        return err(kENOMEM);
    }
    const uint32_t handle = addHandle(file, object);
    if (handle == 0) {
        object->release();
        return err(kENOMEM);
    }
    request.size = bytes;
    request.handle = handle;
    if (!copyOut(arg, &request, size)) {
        file->handles[handle - 1] = nullptr;
        object->release();
        return err(kEFAULT);
    }
    return 0;
}

// GET_RESET_STATS (i915_gem_context_reset_stats_ioctl): how often the
// context's batches were lost. The global count is for privileged callers.
uint64_t doResetStats(DrmFile* file, uint64_t arg, uint32_t size) {
    drm_i915_reset_stats request;
    if (!copyIn(&request, arg, size)) {
        return err(kEFAULT);
    }
    if (request.flags != 0 || request.pad != 0) {
        return err(kEINVAL);
    }
    if (request.ctx_id >= kMaxContexts || !file->contexts[request.ctx_id]) {
        return err(kENOENT);
    }
    const DrmContext* context = file->contexts[request.ctx_id];
    request.reset_count = 0;
    request.batch_active = context->guilty;
    request.batch_pending = 0;
    return copyOut(arg, &request, size) ? 0 : err(kEFAULT);
}

} // namespace

bool available() {
    return gpuPresent() || gAllowWithoutGpu;
}

void allowWithoutGpu() {
    gAllowWithoutGpu = true;
    publishSysfs();   // the same layout, ids zero, for the QEMU checks
}

VNode* createFileNode() {
    if (!available()) {
        return nullptr;
    }
    auto* file = new DrmFile();
    auto* node = new VNode(nullptr, 128, FileType::CharDevice);
    if (file) {
        file->defaultVm = createVm(file);
        file->contexts[0] = file->defaultVm ? newContext(file) : nullptr;
    }
    if (!file || !node || !file->contexts[0]) {
        if (file) {
            destroyFile(file);
        }
        delete node;
        return nullptr;
    }
    node->refCount = 0;
    node->ops = &gRenderNodeOps;
    node->setData(file);
    return node;
}

bool isRenderNode(VNode* node) {
    return node && node->ops == &gRenderNodeOps;
}

bool isDrmNode(VNode* node) {
    return node && (node->ops == &gRenderNodeOps || node->ops == &gDmaBufOps || node->ops == &gSyncFileOps ||
                    node->ops == &gSyncobjFdOps);
}

// poll() on a DRM fd: a sync_file is readable once its fence is signaled; a
// dma-buf readable once nothing writes it and writable once nothing uses it
// (dma-buf.c). The render node and sync object fds are always ready.
int16_t poll(VNode* node, int16_t events) {
    constexpr int16_t kPollIn = 0x1;
    constexpr int16_t kPollOut = 0x4;
    if (gpuPresent()) {
        IntelGen9Driver::get().processRequests();
    }
    int16_t ready = kPollIn | kPollOut;
    if (node->ops == &gSyncFileOps) {
        auto* fence = static_cast<Fence*>(node->getData());
        ready = fence && fence->poll() ? kPollIn : 0;
    } else if (node->ops == &gDmaBufOps) {
        auto* object = static_cast<GemObject*>(node->getData());
        ready = 0;
        if (object && !fenceBusy(object->write)) {
            ready |= kPollIn;
        }
        if (object && objectIdle(object)) {
            ready |= kPollOut;
        }
    }
    return static_cast<int16_t>(ready & events);
}

int64_t exportScanoutPages(const uint64_t* frames, uint32_t count) {
    return exportPagesAsDmaBuf(frames, count);
}

int64_t reexportDmaBuf(VNode* node) {
    if (!node || node->ops != &gDmaBufOps || !node->getData()) {
        return -kEINVAL;
    }
    auto* object = static_cast<GemObject*>(node->getData());
    object->retain();
    VNode* copy = makeNode(&gDmaBufOps, object, FileType::Regular);
    if (!copy) {
        object->release();
        return -kENOMEM;
    }
    const int64_t slot = installNode(copy, false);
    if (slot < 0) {
        return slot;
    }
    return static_cast<int64_t>(HandleTable::encodeHandle(HandleType::File, static_cast<uint32_t>(slot)));
}

uint64_t dmaBufSize(VNode* node) {
    if (!node || node->ops != &gDmaBufOps) {
        return 0;
    }
    auto* object = static_cast<GemObject*>(node->getData());
    return object ? object->pageCount * kPage : 0;
}

bool waitDmaBufWritten(VNode* node, uint64_t timeoutMs) {
    if (!node || node->ops != &gDmaBufOps) {
        return true;
    }
    auto* object = static_cast<GemObject*>(node->getData());
    if (!object) {
        return true;
    }
    if (!gpuPresent()) {
        return !fenceBusy(object->write);
    }
    object->retain();
    const bool done = waitFor([&] { return !fenceBusy(object->write); },
                              time_get_uptime_ns() + timeoutMs * 1000000ull);
    // Mesa draws shared buffers with the external MOCS (not LLC-cached), so
    // its writes went to memory past the CPU caches, and the compositor's
    // write-back mapping would keep showing what it read the first time.
    flushObject(object);
    object->release();
    return done;
}

uint64_t dispatchIoctl(VNode* node, uint64_t request, uint64_t arg) {
    if (node && node->ops == &gDmaBufOps) {
        return doDmaBufIoctl(node, request, arg);
    }
    if (node && node->ops == &gSyncFileOps) {
        return doSyncFileIoctl(node, request, arg);
    }
    if (node && node->ops == &gSyncobjFdOps) {
        return err(kENOTTY);
    }
    DrmFile* file = fileOf(node);
    if (!file || file->closed) {
        return err(kEINVAL);
    }
    if (iocType(request) != kIoctlType) {
        return err(kENOTTY);
    }
    const uint32_t nr = iocNr(request);
    const uint32_t size = iocSize(request);
    switch (nr) {
        case DRM_VERSION_NR:
            return doVersion(arg, size);
        case DRM_GEM_CLOSE_NR:
            return doClose(file, arg, size);
        case kCommandBase + DRM_I915_GETPARAM:
            return doGetparam(arg, size);
        case kCommandBase + DRM_I915_GEM_CREATE:
            return doCreate(file, arg, size);
        case kCommandBase + DRM_I915_GEM_MMAP_GTT:
            return doMmapOffset(file, arg, size);
        case kCommandBase + DRM_I915_GEM_EXECBUFFER2:
            return doExecbuffer(file, arg, size, (iocDir(request) & kIocRead) != 0);
        case kCommandBase + DRM_I915_GEM_CONTEXT_CREATE:
            return doContextCreate(file, arg, size);
        case kCommandBase + DRM_I915_GEM_CONTEXT_DESTROY:
            return doContextDestroy(file, arg, size);
        case kCommandBase + DRM_I915_GEM_SET_DOMAIN:
            return doSetDomain(file, arg, size);
        case kCommandBase + DRM_I915_GEM_SW_FINISH: {
            drm_i915_gem_sw_finish request;
            if (!copyIn(&request, arg, size)) return err(kEFAULT);
            return doHandleOnly(file, request.handle);
        }
        case kCommandBase + DRM_I915_GEM_BUSY:
            return doGemBusy(file, arg, size);
        case kCommandBase + DRM_I915_GEM_WAIT:
            return doGemWait(file, arg, size);
        case kCommandBase + DRM_I915_GET_RESET_STATS:
            return doResetStats(file, arg, size);
        case kCommandBase + DRM_I915_GEM_CONTEXT_GETPARAM:
            return doContextParam(file, arg, size, false);
        case kCommandBase + DRM_I915_GEM_CONTEXT_SETPARAM:
            return doContextParam(file, arg, size, true);
        case kCommandBase + DRM_I915_GEM_VM_CREATE:
            return doVmCreate(file, arg, size);
        case kCommandBase + DRM_I915_GEM_VM_DESTROY:
            return doVmDestroy(file, arg, size);
        case kCommandBase + DRM_I915_QUERY:
            return doQuery(arg, size);
        case kCommandBase + DRM_I915_REG_READ:
            return doRegRead(arg, size);
        case kCommandBase + DRM_I915_GEM_GET_APERTURE:
            return doGetAperture(arg, size);
        case kCommandBase + DRM_I915_GEM_SET_TILING:
            return doSetTiling(file, arg, size);
        case kCommandBase + DRM_I915_GEM_GET_TILING:
            return doGetTiling(file, arg, size);
        case kCommandBase + DRM_I915_GEM_SET_CACHING:
            return doSetCaching(file, arg, size);
        case kCommandBase + DRM_I915_GEM_GET_CACHING:
            return doGetCaching(file, arg, size);
        case kCommandBase + DRM_I915_GEM_MADVISE:
            return doMadvise(file, arg, size);
        case kCommandBase + DRM_I915_GEM_CREATE_EXT:
            return doCreateExt(file, arg, size);
        case DRM_GET_CAP_NR:
            return doGetCap(arg, size);
        case DRM_SYNCOBJ_CREATE_NR:
            return doSyncobjCreate(file, arg, size);
        case DRM_SYNCOBJ_DESTROY_NR:
            return doSyncobjDestroy(file, arg, size);
        case DRM_SYNCOBJ_WAIT_NR:
            return doSyncobjWait(file, arg, size, false);
        case DRM_SYNCOBJ_TIMELINE_WAIT_NR:
            return doSyncobjWait(file, arg, size, true);
        case DRM_SYNCOBJ_RESET_NR:
            return doSyncobjSignal(file, arg, size, 0);
        case DRM_SYNCOBJ_SIGNAL_NR:
            return doSyncobjSignal(file, arg, size, 1);
        case DRM_SYNCOBJ_TIMELINE_SIGNAL_NR:
            return doSyncobjSignal(file, arg, size, 2);
        case DRM_SYNCOBJ_QUERY_NR:
            return doSyncobjQuery(file, arg, size);
        case DRM_SYNCOBJ_TRANSFER_NR:
            return doSyncobjTransfer(file, arg, size);
        case DRM_SYNCOBJ_HANDLE_TO_FD_NR:
            return doSyncobjToFd(file, arg, size);
        case DRM_SYNCOBJ_FD_TO_HANDLE_NR:
            return doSyncobjFromFd(file, arg, size);
        case DRM_PRIME_HANDLE_TO_FD_NR:
            return doPrimeToFd(file, arg, size);
        case DRM_PRIME_FD_TO_HANDLE_NR:
            return doPrimeFromFd(file, arg, size);
        default:
            return err(kEINVAL);
    }
}

// Every DRM ioctl; the first kMaxLoggedFailures that fail are logged with
// the caller, the number, the errno and the start of the argument (for
// GETPARAM and the context parameters, which one), so what a port such as
// Mesa asks and is refused shows in the kernel log.
uint64_t ioctl(VNode* node, uint64_t request, uint64_t arg) {
    constexpr uint32_t kMaxLoggedFailures = 64;
    static uint32_t logged = 0;
    const uint64_t result = dispatchIoctl(node, request, arg);
    if (static_cast<int64_t>(result) < 0 && logged < kMaxLoggedFailures) {
        ++logged;
        uint64_t head[2] = {0, 0};
        const uint32_t size = iocSize(request);
        if (arg != 0 && size != 0) {
            Syscall::copyFromUser(head, arg, size < sizeof(head) ? size : sizeof(head));
        }
        Process* current = Scheduler::get().getCurrentProcess();
        char line[160];
        const char* hex = "0123456789abcdef";
        uint32_t n = 0;
        auto put = [&](const char* text) {
            while (*text && n + 1 < sizeof(line)) line[n++] = *text++;
        };
        auto putHex = [&](uint64_t value, int digits) {
            for (int i = digits - 1; i >= 0 && n + 1 < sizeof(line); --i) line[n++] = hex[(value >> (i * 4)) & 0xF];
        };
        auto putDec = [&](uint64_t value) {
            char digits[24];
            int count = 0;
            do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
            while (count && n + 1 < sizeof(line)) line[n++] = digits[--count];
        };
        put("[drm] pid ");
        putDec(current ? current->getPID() : 0);
        put(": ioctl '");
        if (n + 1 < sizeof(line)) line[n++] = static_cast<char>(iocType(request));
        put("' 0x");
        putHex(iocNr(request), 2);
        put(" failed with errno ");
        putDec(static_cast<uint64_t>(-static_cast<int64_t>(result)));
        put("; argument starts 0x");
        putHex(head[0], 16);
        put(" 0x");
        putHex(head[1], 16);
        put("\n");
        line[n] = '\0';
        log(line);
    }
    return result;
}

uint64_t resolveMmap(VNode* node, uint64_t offset, uint64_t length, MmapTarget* out) {
    if (node && node->ops == &gDmaBufOps) {
        // A dma-buf maps from a byte offset into its object, write-back.
        auto* object = static_cast<GemObject*>(node->getData());
        if (!object || !out || (offset & (kPage - 1)) != 0 || length == 0 || (length & (kPage - 1)) != 0 ||
            offset / kPage + length / kPage > object->pageCount) {
            return err(kEINVAL);
        }
        out->object = object;
        out->pages = object->pages + offset / kPage;
        out->cacheFlags = 0;
        return 0;
    }
    if (!node || node->ops != &gRenderNodeOps) {
        return err(kENODEV);
    }
    DrmFile* file = fileOf(node);
    if (!file || file->closed || !out || (offset & (kPage - 1)) != 0 || length == 0 ||
        (length & (kPage - 1)) != 0) {
        return err(kEINVAL);
    }
    const uint32_t handle = static_cast<uint32_t>(offset >> 32);
    const uint64_t mode = (offset >> kOffsetModeShift) & 0xF;
    const uint64_t firstPage = (offset & kOffsetPageMask) / kPage;
    GemObject* object = lookup(file, handle);
    if (!object) {
        return err(kEINVAL);
    }
    if (firstPage + length / kPage > object->pageCount) {
        return err(kEINVAL);
    }
    uint64_t cache = 0;
    if (mode == I915_MMAP_OFFSET_WC) {
        cache = WriteCombine;
    } else if (mode == I915_MMAP_OFFSET_UC) {
        cache = CacheDisab | WriteThru;
    } else if (mode != I915_MMAP_OFFSET_WB) {
        return err(kEINVAL);
    }
    out->object = object;
    out->pages = object->pages + firstPage;
    out->cacheFlags = cache;
    return 0;
}

} // namespace drm
