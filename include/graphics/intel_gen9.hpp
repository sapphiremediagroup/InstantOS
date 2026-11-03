#pragma once

#include <stddef.h>
#include <stdint.h>
#include <graphics/framebuffer.hpp>
#include <graphics/intel_gen9_logic.hpp>
#include <graphics/intel_gen9_render_logic.hpp>

// Intel HD Graphics 530 / Skylake (Gen9) integrated GPU support.
//
// This driver targets the Intel Gen9 (Skylake) integrated GPU. The flagship
// target is "HD Graphics 530" (Skylake GT2, PCI device 0x1912 desktop and
// 0x191B mobile/halo), but the whole Skylake GT1..GT4 family is recognised.
// QEMU cannot emulate it, so it is tested on a real PC (see CLAUDE.md, "Real
// hardware"); the pure logic is in intel_gen9_logic.hpp with host fixtures.
//
// At boot it only reads: PCI detection, the BARs (GTTMMADR: registers and the
// GTT; GMADR: the CPU's aperture into GTT space), a register dump, and the
// firmware's display plane, whose linear scanout becomes the framebuffer.
// Everything that writes to the GPU runs on request from /bin/gpu-test
// (the GpuTest syscall), never at boot:
//   * the global GTT (GGTT): mapped, the firmware's entries reclaimed as i915
//     does except the scanned-out plane (and the top 2 MiB), a page allocator,
//     freed entries on a scratch page;
//   * buffer objects: system RAM pages bound into the GGTT, reachable by the
//     CPU through the aperture (write-combining) or the direct map;
//   * a plane flip onto a buffer object (the visible check that the GGTT works);
//   * the render and blitter engines (intel_gen9_engine.cpp).
// The one exception is presentation (intel_gen9_display.cpp): once the
// graphics-compositor asks for it (the Display syscall), it draws into two
// scanout buffers and plane A flips between them at vertical blank.
// Not yet: modesetting.

class Process;

namespace intel_gen9 {

// PCI class for a VGA-compatible display controller (class 0x03, subclass 0x00).
constexpr uint8_t PCI_CLASS_DISPLAY = 0x03;
constexpr uint8_t PCI_SUBCLASS_VGA = 0x00;

// MMIO / BAR layout ----------------------------------------------------------
//
// On Gen9 the PCI BARs of the IGD (function 0:2.0) are:
//   BAR0 (0x10): GTTMMADR - 16 MiB: registers in the lower half, the GTT in the
//                upper half (GSM_OFFSET_IN_BAR0).
//   BAR2 (0x18): GMADR    - the graphics aperture (mappable through the GTT).
//   BAR4 (0x20): I/O BAR  - legacy index/data I/O port pair.
constexpr uint16_t PCI_BAR0_OFFSET = 0x10; // GTTMMADR low dword.
constexpr uint16_t PCI_BAR2_OFFSET = 0x18; // GMADR low dword.
constexpr uint16_t PCI_BAR4_OFFSET = 0x20; // I/O BAR.

// The register block mapped at boot (the first 2 MiB of GTTMMADR).
constexpr uint64_t MMIO_REG_SIZE = 2ull * 1024 * 1024;

// The CPU only uses the first 256 MiB of the aperture: every Gen9 GMADR is at
// least that large, so its size need not be probed on a live device.
constexpr uint64_t APERTURE_USABLE_BYTES = 256ull * 1024 * 1024;

// Display engine registers (Gen9 / Skylake), relative to the register window.
constexpr uint32_t REG_PIPE_A_BASE = 0x60000;

// PIPE_SRCSZ_A: horizontal/vertical source size of pipe A's active region.
//   bits [28:16] = width-1, bits [12:0] = height-1.
constexpr uint32_t REG_PIPE_SRCSZ_A = 0x6001C;

// Universal plane 1 (primary) on pipe A.
constexpr uint32_t REG_PLANE_CTL_A = 0x70180;     // enable + pixel format.
constexpr uint32_t REG_PLANE_STRIDE_A = 0x70188;  // stride in 64-byte tiles/units.
constexpr uint32_t REG_PLANE_SIZE_A = 0x70190;    // (height-1)<<16 | (width-1).
constexpr uint32_t REG_PLANE_SURF_A = 0x7019C;    // surface base addr (in GMADR/GTT).
constexpr uint32_t REG_PLANE_SURFLIVE_A = 0x701AC; // the surface being scanned out.

enum class EngineId : uint8_t {
    Render = 0,
    Blitter = 1,
};
constexpr int kEngineCount = 2;

// A buffer object: system RAM pages bound at one place in the GGTT.
struct GpuBuffer {
    uint64_t gttOffset;     // where the GPU sees it
    uint64_t size;          // bytes, whole pages
    uint32_t pageCount;
    uint64_t* pages;        // physical address of each page (kmalloc'd)
    void* aperture;         // CPU pointer through the aperture (WC), or nullptr
};

// A per-process GPU address space: a 48-bit, 4-level PPGTT (i915's
// gen8_ppgtt). Ranges nobody mapped lead through shared scratch tables to
// the scratch page, as in i915, so a stray GPU access reads zeros instead of
// faulting. Tables are allocated as mappings need them.
struct Ppgtt {
    uint64_t pml4;          // physical address of the top-level table
    uint32_t tablePages;    // tables allocated for it (the PML4 included)
};

// A GPU context that keeps its state from one batch to the next (i915's
// intel_context on execlists): its image (the per-process status page and
// the register state the engine saves and restores) and a ring of its own,
// both in the GGTT. Its first run builds the register state with restore
// inhibited and, on the render engine, sets the context workarounds and runs
// the null-state batch, as i915 does for a new context; later runs restore
// what the engine saved and only move the ring window. A DRM context holds
// one per engine.
struct HwContext {
    GpuBuffer* image = nullptr;
    GpuBuffer* ring = nullptr;
    uint32_t tail = 0;          // the ring's next free byte
    uint32_t hwId = 0;          // the execlist descriptor's context id
    bool started = false;       // has run (and been saved) at least once
};

// A fence (i915's dma_fence on a request): signaled once its request has
// retired, with the request's outcome. Whoever keeps or waits on it holds a
// reference: the request, buffer objects (their last reader per engine and
// last writer), sync objects.
struct Fence {
    uint32_t refs = 1;
    bool signaled = false;
    int status = 0;             // 0, or -EIO: the batch hung, or was skipped (banned context, failed dependency)
    // A join (a merged sync_file, a timeline point standing for every point
    // before it, a dma-buf's fences): signaled once all its parts are, with
    // the first error among them. Its parts are never joins themselves.
    Fence** parts = nullptr;    // referenced, new[]'d
    uint32_t partCount = 0;

    void retain() { ++refs; }
    void release() {
        if (--refs == 0) {
            for (uint32_t i = 0; i < partCount; ++i) {
                parts[i]->release();
            }
            delete[] parts;
            delete this;
        }
    }
    // Whether it is signaled; a join looks at its parts. Read it through
    // this, not `signaled`, wherever a fence may be a join.
    bool poll() {
        if (signaled || !parts) {
            return signaled;
        }
        for (uint32_t i = 0; i < partCount; ++i) {
            if (!parts[i]->signaled) {
                return false;
            }
        }
        for (uint32_t i = 0; i < partCount && status == 0; ++i) {
            status = parts[i]->status;
        }
        signaled = true;
        return true;
    }
};

// One batch to run (i915's i915_request): queued per engine, run one at a
// time in order once every fence in `deps` is signaled, and retired when the
// breadcrumb has landed and the engine has saved the context. The owner
// fills the first part and gets it back through `retire` (with `done`
// already signaled) to release what it holds.
struct Request {
    EngineId engine;
    HwContext* context;
    const Ppgtt* vm;
    uint64_t batchVa;
    Fence* done;                // referenced by the request
    Fence** deps;               // referenced; may be nullptr when depCount is 0
    uint32_t depCount;
    const bool* skip;           // when this reads true at dispatch, the request is not run (-EIO)
    void (*retire)(Request*);
    void* owner;
    // The driver's.
    bool hung;                  // it was running when the engine was reset
    uint32_t seqno;
    uint64_t idleBefore;        // the engine's idle events when it was dispatched
    uint64_t queuedNs;
    uint64_t startedNs;
    Request* next;
};

class IntelGen9Driver {
public:
    static IntelGen9Driver& get();

    // Probe the PCI bus for a supported Gen9 part. Cheap; no MMIO touched.
    bool probe();

    // Boot bring-up, read-only on the GPU: enable PCI memory decode + bus
    // mastering, map the registers, dump them, and adopt the firmware's linear
    // scanout. The fallback framebuffer gives the geometry when the plane
    // cannot be interpreted. False (without changing anything) on any
    // unsupported or invalid condition.
    bool initialize(iFramebuffer& fallback);

    bool isInitialized() const { return initialized; }
    bool isPresent() const { return deviceFound; }

    const Gen9DeviceInfo* deviceInfo() const { return info; }
    // The PCI revision id (I915_PARAM_REVISION).
    uint8_t pciRevision() const { return static_cast<uint8_t>(readConfig32(0x08) & 0xFF); }
    // Its configuration space (for /sys).
    uint32_t pciConfigDword(uint16_t offset) const { return readConfig32(offset); }
    const DisplayMode& mode() const { return display; }

    // The active scanout framebuffer (CPU-visible pointer): the firmware/GOP
    // framebuffer the bootloader handed us.
    void* getFramebuffer() const { return framebuffer; }
    uint32_t getWidth() const { return display.width; }
    uint32_t getHeight() const { return display.height; }
    uint32_t getPitch() const { return display.stride; }
    uint32_t getFramebufferSize() const { return fbSize; }

    // The inherited scanout is CPU-coherent, so this is a no-op success.
    bool flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

    uint8_t pciBus() const { return bus; }
    uint8_t pciDevice() const { return device; }
    uint8_t pciFunction() const { return function; }

    uint64_t mmioBase() const { return gttmmadr; }
    uint64_t apertureBase() const { return gmadr; }

    uint32_t readReg(uint32_t offset) const;
    void writeReg(uint32_t offset, uint32_t value);

    // GGTT (intel_gen9_gtt.cpp) -----------------------------------------------

    // Maps the GTT, checks it against the firmware's plane, reserves what the
    // firmware uses and points a scratch page into nothing. Idempotent. A
    // report of what it found goes to the kernel log.
    bool initializeGtt();
    bool gttReady() const { return gttInitialized; }

    // A buffer object of at least `bytes`, bound in the GGTT at an offset
    // aligned to `alignment` (a power of two, at least 4 KiB). With
    // `mappable`, it lies in the CPU-usable aperture and gets an aperture
    // pointer. The pages are zeroed and flushed from the CPU's caches.
    GpuBuffer* createBuffer(uint64_t bytes, uint64_t alignment, bool mappable);
    // Unbinds (entries back to the scratch page), unmaps and frees it.
    void destroyBuffer(GpuBuffer* buffer);

    uint64_t gttFreeBytes() const;
    uint64_t gttTotalBytes() const;
    // For the DRM queries (intel_gen9_wa.cpp): the fuses, read once
    // (forcewake taken); nullptr if the GPU cannot be woken.
    const Gen9Sseu* topology();
    // The command streamer's timestamp rate in Hz (CTC_MODE, as i915's
    // gen9_read_clock_frequency).
    uint32_t timestampFrequency();
    // REG_READ: the render engine's RING_TIMESTAMP, read as two dwords.
    bool readRenderTimestamp(uint64_t* value);

    // Points plane A at a GGTT offset (the frame shown from the next vblank)
    // and waits up to 100 ms for the display to take it; true once it did.
    bool flipPlane(uint64_t gttOffset);
    uint64_t firmwareSurface() const { return display.surfaceGtt; }

    // Presentation for the compositor (intel_gen9_display.cpp) ----------------

    static constexpr uint32_t kScanoutBuffers = 2;
    // Creates the scanout buffers once (screen-sized, the firmware plane's
    // stride) and maps them write-combining into `owner`, buffer i at
    // userBase + i * userStride; their addresses go to `addresses`.
    bool displayAcquire(Process* owner, uint64_t userBase, uint64_t userStride, uint64_t* addresses);
    // A scanout buffer (made by displayAcquire) as a dma-buf of the calling
    // process: its handle, or -errno.
    int64_t displayExportBuffer(uint32_t index);
    // Arms plane A to show buffer `index` from the next vertical blank.
    bool displayFlip(uint32_t index);
    // Sleeps until the last armed flip is on screen; false after `ms`.
    bool displayWait(uint64_t ms);
    // The firmware's framebuffer back on screen (the compositor gave up).
    void displayRelease();
    // A process ended: if it owned the display, the firmware's framebuffer
    // goes back on screen (the buffers stay, for a restarted compositor).
    void displayProcessGone(uint32_t pid);
    // gpu-test's flip: while its pattern shows, the compositor's flips are
    // only recorded; the end puts back what the compositor last showed.
    void displayTestBegin();
    bool displayTestEnd();
    // Panic, kernel exception and watchdog paths: they draw into the
    // firmware's framebuffer, so it goes on screen. No lock, MMIO only.
    void showFirmwareSurfaceUnlocked();
    // The watchdog's stall is over: the compositor's buffer goes back on
    // screen (the compositor's flips were only recorded meanwhile).
    void restorePresentedSurfaceUnlocked();

    // Engines (intel_gen9_engine.cpp) ------------------------------------------

    // The engine runs a ring that stores two values into memory
    // (MI_STORE_DWORD_IMM, then MI_FLUSH_DW's post-sync write on the blitter,
    // a second store on the render engine), which the CPU checks. The engine
    // is reset first, so every run starts clean.
    bool engineStoreTest(EngineId id);
    // A ring on the blitter that waits on a semaphore nobody releases: the
    // wait must be seen as a hang, the engine reset, and the stores work after.
    bool engineHangTest();
    // Each engine stores a value and raises MI_USER_INTERRUPT; the caller
    // sleeps until the GPU's MSI arrives (no polling of the memory). The
    // interrupts are switched off again afterwards.
    bool interruptTest();

    // PPGTT (intel_gen9_ppgtt.cpp) ---------------------------------------------

    // Writes i915's private PAT (kGen9PrivatePat); needs forcewake.
    void programPrivatePat();
    Ppgtt* createPpgtt();
    // Every table and the PML4; the pages mapped are the caller's.
    void destroyPpgtt(Ppgtt* vm);
    // Maps `buffer`'s pages at `va` (page aligned, below 2^48), cached in the
    // LLC (coherent with the CPU) or uncached.
    bool ppgttMap(Ppgtt* vm, uint64_t va, const GpuBuffer* buffer, bool cached);
    // Points [va, va + bytes) back at the scratch page (tables stay).
    void ppgttUnmap(Ppgtt* vm, uint64_t va, uint64_t bytes);
    // On each engine, a batch in a PPGTT at a high address stores into a
    // buffer mapped there; the CPU writes the batch and reads the result
    // through its own cached mapping, without flushing (coherence).
    bool ppgttTest();

    // User submissions (EXECBUFFER2 on /dev/dri/renderD128) -------------------

    // A gpu-test experiment takes the engines: once no request is queued or
    // running (sleeping up to `waitMs` for that, the big kernel lock
    // released); requests queued meanwhile wait until releaseEngines.
    bool claimEngines(uint64_t waitMs);
    void releaseEngines();

    // Queues `request` on its engine (the first time also preparing the
    // engine: workarounds, default context, interrupts) and starts it if
    // the engine is free; it then runs and retires on its own (from the
    // GPU's interrupt, or whenever processRequests runs). 0, or -EIO when
    // the engine could not be prepared (nothing queued).
    int queueRequest(Request* request);
    // Retires what finished, catches a request that hung (no completion in
    // kHangMs: the engine is reset, the request fails with -EIO, its
    // context's state is lost), and starts what can start. Called from the
    // interrupt handler and by anyone waiting.
    void processRequests();
    uint32_t queuedRequests() const { return queuedCount; }
    // For waiters: a counter that moves on every GPU interrupt, and a sleep
    // (the big kernel lock released) until it moves past `seen` or `ms` pass.
    uint64_t gpuEvents() const { return interruptCount; }
    void sleepForGpu(uint64_t seen, uint64_t ms);
    static constexpr uint64_t kHangMs = 2000;
    // From the boot CPU's tick: catches a hang nobody is waiting on.
    void timerTick();
    // gpu-test state: the queue, the engines and what the requests took, as
    // log lines (forcewake taken for the registers).
    void logRequestState();
    // A context's image and ring (GGTT buffers); false if out of space.
    bool createHwContext(EngineId id, HwContext& context);
    void destroyHwContext(HwContext& context);

    // From the GPU's MSI (gpu interrupt handler): acknowledges what the GT
    // raised and wakes sleepers.
    void handleInterrupt();

private:
    IntelGen9Driver() = default;

    bool detectDevice();
    bool mapBars();
    // Read-only register dump for bring-up on real hardware (nothing is
    // written): PCI GTT/stolen config, display pipes, engine rings, CPU PAT.
    void logDiagnostics();
    bool recoverDisplayMode(iFramebuffer& fallback);

    uint16_t readConfig16(uint16_t offset) const;
    uint32_t readConfig32(uint16_t offset) const;
    void writeConfig16(uint16_t offset, uint16_t value);

    // GGTT helpers (intel_gen9_gtt.cpp).
    uint64_t readPte(uint32_t index) const;
    void writePte(uint32_t index, uint64_t pte);
    void invalidateGtt();
    void reserveFirmwareRanges();

    // Engine helpers (intel_gen9_engine.cpp).
    // Counted: the first get wakes the GT, the last put lets it sleep.
    bool forcewakeGet();
    void forcewakePut();
    bool forcewakeWake();
    void forcewakeRelease();
    struct EngineState;
    EngineState& engine(EngineId id);
    bool resetEngine(EngineState& e);
    bool setUpEngine(EngineState& e);
    // Builds a fresh context around `dwords` of ring commands and submits it
    // to the engine; returns the uptime (ms) of the submission, for timings.
    // `verbose` logs the submission (gpu-test's experiments; not user batches).
    uint64_t submit(EngineState& e, const uint32_t* dwords, uint32_t count, const Ppgtt* vm = nullptr,
                    bool verbose = true);
    bool runPpgttStores(EngineState& e);
    // Rendering setup (intel_gen9_wa.cpp), with forcewake held: the private
    // PAT, the fuses, the GT workarounds and the relocated null-state batch,
    // once; false if a buffer could not be made.
    bool prepareRendering();
    // The engine's own workarounds and, on the render engine, the register
    // whitelist: lost in an engine reset, so set after every one (setUpEngine).
    void applyEngineWorkarounds(EngineState& e);
    // Request plumbing (intel_gen9_engine.cpp).
    bool prepareEngine(EngineState& e, EngineId id);
    void dispatch(EngineState& e, Request* request);
    bool activeDone(EngineState& e);
    // Reads the context status buffer's new events (and moves its read
    // pointer, as i915 does); returns how many were switches to idle.
    uint32_t drainCsb(EngineState& e);
    // Polls (interrupts off) until the engine's last event is a switch to
    // idle: the context it ran is saved.
    bool waitEngineIdle(EngineState& e, uint64_t ms);
    // The active request in a hang report: breadcrumb, ring window, the ring around HEAD.
    void logActiveRequest(EngineState& e);
    void finishActive(EngineState& e, int status);
    // Compares the context workarounds a context's first run read back.
    void verifyContextWorkarounds();
    void writeElsp(EngineState& e, uint64_t descriptor);
    // The engine's default context image (recordDefaultContext), which new
    // contexts are copies of; engines claimed, forcewake held.
    bool recordDefaultContext(EngineId id);
    // PPGTT helpers (intel_gen9_ppgtt.cpp).
    bool ensurePpgttScratch();
    uint64_t* ppgttTable(uint64_t phys);
    uint64_t ppgttAllocTable(uint64_t fill);
    void ppgttFreeLevel(uint64_t tablePhys, uint32_t level);
    bool runStores(EngineState& e);
    void logEngineState(const EngineState& e);
    bool enableInterrupts();
    void disableInterrupts();
    // Sleeps (the big kernel lock released) until `counter` moves or `ms` pass.
    bool sleepUntilChanged(volatile uint64_t& counter, uint64_t from, uint64_t ms);
    void writeBufferDword(GpuBuffer* buffer, uint64_t offset, uint32_t value);
    uint32_t readBufferDword(GpuBuffer* buffer, uint64_t offset);

    bool deviceFound = false;
    bool initialized = false;

    uint8_t bus = 0;
    uint8_t device = 0;
    uint8_t function = 0;
    const Gen9DeviceInfo* info = nullptr;

    uint64_t gttmmadr = 0;            // BAR0 base (MMIO + GTT).
    volatile uint8_t* mmio = nullptr; // mapped register window.
    uint64_t gmadr = 0;               // BAR2 base (aperture).

    DisplayMode display = {};
    void* framebuffer = nullptr;
    uint32_t fbSize = 0;

    // GGTT state.
    bool gttInitialized = false;
    GgcInfo ggc = {};
    uint64_t stolenBase = 0;
    volatile uint64_t* gsm = nullptr;   // the GTT's entries (WC mapping)
    uint32_t gttEntries = 0;
    uint64_t scratchPage = 0;           // physical; where unused entries point
    // The PPGTT's scratch chain (physical): a PT, PD and PDP whose every
    // entry leads to the next one down and finally to scratchPage.
    uint64_t scratchPt = 0;
    uint64_t scratchPd = 0;
    uint64_t scratchPdp = 0;
    GttPageAllocator allocator;         // its bitmap is kmalloc'd by initializeGtt

    // Engine state, kept between runs (setUpEngine).
    struct EngineState {
        const char* name;
        uint32_t base;              // MMIO base of its registers
        uint32_t resetDomain;       // its GDRST bit
        const uint8_t* layout;      // its context image layout
        uint64_t contextBytes;      // per-process status page + register state
        uint32_t irqShift;          // its bits in GT interrupt bank 0
        GpuBuffer* statusPage;      // RING_HWS_PGA
        GpuBuffer* context;
        GpuBuffer* ring;
        GpuBuffer* target;          // what the commands write
        bool userReady = false;     // set up since its last reset (runBatch)
        bool workaroundsApplied = false;   // applyEngineWorkarounds since its last reset
        bool mocsLogged = false;
        uint8_t* defaultImage = nullptr;   // the recorded default context (contextBytes, kmalloc'd)
        Request* queueHead = nullptr;      // waiting, in order
        Request* queueTail = nullptr;
        Request* active = nullptr;         // on the engine
        uint32_t csbRead = CSB_ENTRIES - 1;  // the last status buffer entry read (drainCsb)
        bool csbIdle = true;               // its last event was a switch to idle
        uint64_t idleEvents = 0;           // switches to idle read so far
    };
    EngineState engines[kEngineCount] = {
        {"render", RENDER_RING_BASE, GRDOM_RENDER, kGen9RcsOffsets, 22 * GTT_PAGE_SIZE, RCS_IRQ_SHIFT,
         nullptr, nullptr, nullptr, nullptr},
        {"blitter", BLT_RING_BASE, GRDOM_BLT, kGen9XcsOffsets, 4 * GTT_PAGE_SIZE, BCS_IRQ_SHIFT,
         nullptr, nullptr, nullptr, nullptr},
    };
    bool forcewakeReset = false;
    uint32_t forcewakeCount = 0;
    uint32_t nextContextId = 1;
    bool patProgrammed = false;         // programPrivatePat ran (it survives engine resets)
    bool renderingPrepared = false;     // prepareRendering
    Gen9Sseu sseu = {};
    bool sseuRead = false;              // sseu holds the fuses
    uint32_t timestampHz = 0;
    uint32_t rpcs = 0;                  // the render contexts' R_PWR_CLK_STATE
    Workaround contextWorkarounds[kSklContextWorkaroundCount + 1] = {};
    uint32_t contextWorkaroundCount = 0;
    GpuBuffer* nullState = nullptr;     // the relocated null-state batch
    GpuBuffer* indirectCtx = nullptr;   // the render contexts' restore-time batch (emitGen9IndirectCtx)
    uint32_t indirectCtxBytes = 0;
    GpuBuffer* workaroundCheck = nullptr;   // a context's first run stores its workaround registers here
    bool contextWorkaroundsChecked = false;
    bool enginesClaimed = false;        // claimEngines
    uint32_t userSeqno = 0;             // the value a request's breadcrumb stores
    uint32_t queuedCount = 0;           // requests queued or running, both engines
    bool requestForcewake = false;      // forcewake held for running requests
    bool processing = false;            // processRequests is on the stack
    bool inInterrupt = false;           // processRequests runs from the MSI handler
    // What requests took, for gpu-test state (since boot).
    struct RequestStats {
        uint64_t dispatched = 0;
        uint64_t retiredByInterrupt = 0;
        uint64_t retiredByWaiter = 0;
        uint64_t failed = 0;            // hung, or the engine could not be woken
        uint64_t skipped = 0;           // a banned context or a failed dependency
        uint64_t runNsTotal = 0;        // dispatch to retire
        uint64_t runNsMax = 0;
        uint64_t waitNsMax = 0;         // queued to dispatched
        uint64_t forcewakeWakes = 0;
        uint64_t forcewakeNsTotal = 0;
        uint64_t forcewakeNsMax = 0;
    } stats;

    // Presentation (intel_gen9_display.cpp).
    GpuBuffer* scanout[kScanoutBuffers] = {};
    uint32_t displayOwner = 0;          // pid of the compositor, 0 if none
    uint64_t presentedSurface = ~0ULL;  // what plane A shows outside a test (~0: the firmware's)
    bool displayTestActive = false;
    volatile bool emergencyShown = false;  // showFirmwareSurfaceUnlocked put it up

    // Interrupts (MSI, registered by the first interruptTest).
    bool msiRegistered = false;
    uint8_t msiVector = 0;
    volatile uint64_t userInterrupts[kEngineCount] = {};
    volatile uint64_t contextSwitches[kEngineCount] = {};   // context switch interrupts
    bool interruptsOn = false;
    volatile uint64_t lastInterruptMs[kEngineCount] = {};
    volatile uint64_t interruptCount = 0;
    volatile uint32_t lastInterruptCpu = 0;
    volatile uint64_t foreignInterrupts = 0;   // master bits outside the GT engines
    volatile uint32_t lastIir = 0;
};

} // namespace intel_gen9
