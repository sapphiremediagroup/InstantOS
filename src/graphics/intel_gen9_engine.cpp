// Gen9 command submission on the render and blitter engines (see
// intel_gen9.hpp): power wells kept awake with forcewake, execlists without
// the GuC, one fresh context per run, engine reset, and the GT's interrupts
// over MSI. Follows Linux's i915 (MIT): intel_uncore.c (forcewake),
// intel_execlists_submission.c (enable_execlists, execlists_submit_ports),
// intel_lrc.c (the context images, through intel_gen9_logic.hpp),
// intel_reset.c (gen8_engine_reset_prepare, gen6_hw_domain_reset) and
// intel_gt_irq.c / i915_irq.c (gen8_gt_irq_postinstall, gen8_irq_handler).
// Runs only on request (the GpuTest syscall).

#include <graphics/intel_gen9.hpp>

#include <cpu/acpi/pci.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/process/process.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/percpu.hpp>
#include <cpu/smp.hpp>
#include <cpu/tlb.hpp>
#include <memory/heap.hpp>
#include <memory/vmm.hpp>
#include <common/string.hpp>
#include <time/tsc_timer.hpp>

namespace intel_gen9 {

namespace {

struct ForcewakeDomain {
    uint32_t request;
    uint32_t ack;
    const char* name;
};

constexpr ForcewakeDomain kForcewakeDomains[] = {
    {FORCEWAKE_RENDER_GEN9, FORCEWAKE_ACK_RENDER_GEN9, "render"},
    {FORCEWAKE_GT_GEN9, FORCEWAKE_ACK_GT_GEN9, "gt"},
    {FORCEWAKE_MEDIA_GEN9, FORCEWAKE_ACK_MEDIA_GEN9, "media"},
};

constexpr uint64_t kForcewakeAckMs = 50;     // i915's FORCEWAKE_ACK_TIMEOUT_MS
constexpr uint64_t kRingBytes = GTT_PAGE_SIZE;
constexpr uint64_t kRunMs = 500;
constexpr uint64_t kHangMs = 300;

constexpr uint32_t kStoreValue = 0xC0FFEE01;
constexpr uint32_t kSecondValue = 0xC0FFEE02;
constexpr uint32_t kNeverValue = 0xDEADBEEF;
constexpr uint32_t kInterruptValue = 0x1A7E0001;
// Where in the target page each command writes.
constexpr uint64_t kStoreOffset = 0;
constexpr uint64_t kSecondOffset = 8;         // MI_FLUSH_DW's address is qword aligned
constexpr uint64_t kSemaphoreOffset = 16;
constexpr uint64_t kInterruptOffset = 24;

// The GT interrupts this driver takes, in each engine's bits.
// The context switch interrupt comes with each context status buffer
// event, so also when a context has been saved (what a request's retirement waits for).
constexpr uint32_t kEngineInterrupts = GT_RENDER_USER_INTERRUPT | GT_CS_MASTER_ERROR_INTERRUPT |
                                       GT_CONTEXT_SWITCH_INTERRUPT;
// After this many interrupts from sources this driver does not serve (the
// display, the PCU), the master stays off rather than interrupting forever.
constexpr uint64_t kMaxForeignInterrupts = 64;

void log(const char* text) {
    Cereal::get().write(text);
}

void logHex(uint64_t value) {
    char buf[19] = "0x";
    for (int i = 0; i < 16; ++i) {
        const uint32_t digit = static_cast<uint32_t>(value >> (60 - 4 * i)) & 0xF;
        buf[2 + i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    buf[18] = '\0';
    log(buf);
}

void logHex32(uint32_t value) {
    char buf[11] = "0x";
    for (int i = 0; i < 8; ++i) {
        const uint32_t digit = (value >> (28 - 4 * i)) & 0xF;
        buf[2 + i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    buf[10] = '\0';
    log(buf);
}

void logDec(uint64_t value) {
    char buf[21];
    int pos = 20;
    buf[pos] = '\0';
    do {
        buf[--pos] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && pos > 0);
    log(&buf[pos]);
}

void logReg(const char* name, uint32_t value) {
    log(" ");
    log(name);
    log("=");
    logHex32(value);
}

// Polls `done` for up to `ms` (interrupts are off: the TSC keeps the time).
template <typename Done>
bool waitMs(uint64_t ms, Done done) {
    const uint64_t deadline = time_get_uptime_ms() + ms;
    while (true) {
        if (done()) {
            return true;
        }
        if (time_get_uptime_ms() >= deadline) {
            return done();
        }
        TLB::pollPending();
        asm volatile("pause");
    }
}

volatile uint32_t* bufferDword(GpuBuffer* buffer, uint64_t offset) {
    auto* page = reinterpret_cast<volatile uint8_t*>(VMM::PhysToVirt(buffer->pages[offset / GTT_PAGE_SIZE]));
    return reinterpret_cast<volatile uint32_t*>(page + offset % GTT_PAGE_SIZE);
}

void flushLine(volatile void* address) {
    asm volatile("clflush (%0)" : : "r"(address) : "memory");
    asm volatile("mfence" ::: "memory");
}

void flushPages(GpuBuffer* buffer) {
    for (uint32_t page = 0; page < buffer->pageCount; ++page) {
        auto* line = reinterpret_cast<volatile uint8_t*>(VMM::PhysToVirt(buffer->pages[page]));
        for (uint64_t offset = 0; offset < GTT_PAGE_SIZE; offset += 64) {
            asm volatile("clflush (%0)" : : "r"(line + offset) : "memory");
        }
    }
    asm volatile("mfence" ::: "memory");
}

// The GPU's MSI vector is its own, so the EOI is sent here.
class GpuInterrupt : public Interrupt {
public:
    void initialize() override {
    }

    void Run(InterruptFrame* frame) override {
        (void)frame;
        sendEOI();
        IntelGen9Driver::get().handleInterrupt();
    }
};

GpuInterrupt& gpuInterrupt() {
    static GpuInterrupt handler;
    return handler;
}

int engineIndex(EngineId id) {
    return static_cast<int>(id);
}

// The CPU whose LAPIC the GPU's MSI is addressed to (the MSI address register
// holds its LAPIC ID in bits 19:12), or -1.
int msiTargetCpu(uint8_t bus, uint8_t device, uint8_t function) {
    PCI& pci = PCI::get();
    uint8_t cap = static_cast<uint8_t>(pci.readConfig16(0, bus, device, function, 0x34) & 0xFC);
    for (int guard = 0; cap != 0 && guard < 32; ++guard) {
        const uint16_t header = pci.readConfig16(0, bus, device, function, cap);
        if ((header & 0xFF) == 0x05) {
            const uint32_t address = pci.readConfig32(0, bus, device, function, cap + 4);
            const uint32_t lapicId = (address >> 12) & 0xFF;
            for (uint32_t i = 0; i < SMP::kMaxCPUs; ++i) {
                PerCPUData* cpu = SMP::cpu(i);
                if (cpu && cpu->lapicId == lapicId) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }
        cap = static_cast<uint8_t>((header >> 8) & 0xFC);
    }
    return -1;
}

// Moves the calling thread onto `cpu` (as SchedSetAffinity does) and logs
// that CPU's LAPIC: which vectors are in service (ISR) or waiting (IRR), and
// the priority (PPR) below which nothing is delivered. A vector left in
// service by a handler that never sent its EOI blocks every vector of the
// same priority class (vector / 16) or lower on that CPU.
void logLapicOf(int cpu) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || cpu < 0) {
        return;
    }
    const uint64_t saved = current->getCpuAffinity();
    current->setCpuAffinity(1ULL << cpu);
    for (int tries = 0; tries < 8 && currentCpuIndex() != static_cast<uint32_t>(cpu); ++tries) {
        Scheduler::get().yield();
    }
    if (currentCpuIndex() == static_cast<uint32_t>(cpu)) {
        LAPIC& lapic = LAPIC::get();
        log("[i915] cpu ");
        logDec(static_cast<uint64_t>(cpu));
        logReg("TPR", lapic.read(0x80));
        logReg("PPR", lapic.read(0xA0));
        log(" ISR=");
        for (int i = 7; i >= 0; --i) {
            logHex32(lapic.read(0x100 + 0x10 * i));
            log(i ? "." : "");
        }
        log(" IRR=");
        for (int i = 7; i >= 0; --i) {
            logHex32(lapic.read(0x200 + 0x10 * i));
            log(i ? "." : "");
        }
        log("\n");
    } else {
        log("[i915] could not move onto the MSI's cpu to read its LAPIC\n");
    }
    current->setCpuAffinity(saved);
}

} // namespace

IntelGen9Driver::EngineState& IntelGen9Driver::engine(EngineId id) {
    return engines[engineIndex(id)];
}

void IntelGen9Driver::writeBufferDword(GpuBuffer* buffer, uint64_t offset, uint32_t value) {
    volatile uint32_t* dword = bufferDword(buffer, offset);
    *dword = value;
    flushLine(dword);
}

uint32_t IntelGen9Driver::readBufferDword(GpuBuffer* buffer, uint64_t offset) {
    volatile uint32_t* dword = bufferDword(buffer, offset);
    flushLine(dword);   // drop a stale line: the GPU wrote to memory
    return *dword;
}

// Keeps the render, GT and media power wells awake (i915's fw_domains_get:
// wait for the ack to clear, request, wait for the ack). The first time,
// every request bit is cleared as i915's fw_domains_reset does.
bool IntelGen9Driver::forcewakeGet() {
    if (forcewakeCount > 0) {
        ++forcewakeCount;   // already awake: the ack stays set while we hold it
        return true;
    }
    const uint64_t start = time_get_uptime_ns();
    if (!forcewakeWake()) {
        return false;
    }
    const uint64_t took = time_get_uptime_ns() - start;
    ++stats.forcewakeWakes;
    stats.forcewakeNsTotal += took;
    if (took > stats.forcewakeNsMax) {
        stats.forcewakeNsMax = took;
    }
    forcewakeCount = 1;
    return true;
}

void IntelGen9Driver::forcewakePut() {
    if (forcewakeCount == 0 || --forcewakeCount > 0) {
        return;
    }
    forcewakeRelease();
}

bool IntelGen9Driver::forcewakeWake() {
    if (!forcewakeReset) {
        for (const ForcewakeDomain& d : kForcewakeDomains) {
            writeReg(d.request, maskedDisable(0xffff));
        }
        forcewakeReset = true;
    }
    for (const ForcewakeDomain& d : kForcewakeDomains) {
        if (!waitMs(kForcewakeAckMs, [&] { return (readReg(d.ack) & FORCEWAKE_KERNEL) == 0; })) {
            log("[i915] forcewake ");
            log(d.name);
            log(": ack still set ");
            logHex32(readReg(d.ack));
            log("\n");
        }
        writeReg(d.request, maskedEnable(FORCEWAKE_KERNEL));
        if (!waitMs(kForcewakeAckMs, [&] { return (readReg(d.ack) & FORCEWAKE_KERNEL) != 0; }) ||
            readReg(d.ack) == 0xFFFFFFFFu) {
            log("[i915] forcewake ");
            log(d.name);
            log(": no ack ");
            logHex32(readReg(d.ack));
            log("\n");
            forcewakeRelease();
            return false;
        }
    }
    return true;
}

void IntelGen9Driver::forcewakeRelease() {
    for (const ForcewakeDomain& d : kForcewakeDomains) {
        writeReg(d.request, maskedDisable(FORCEWAKE_KERNEL));
    }
}

// Stops the command streamer, asks the engine to get ready, then resets its
// domain through GDRST (twice, as i915 does before Meteor Lake).
bool IntelGen9Driver::resetEngine(EngineState& e) {
    e.userReady = false;
    e.workaroundsApplied = false;
    const uint32_t base = e.base;
    writeReg(base + RING_MI_MODE, maskedEnable(STOP_RING));
    const bool idle = waitMs(100, [&] { return (readReg(base + RING_MI_MODE) & MODE_IDLE) != 0; });

    const uint32_t control = readReg(base + RING_RESET_CTL);
    uint32_t request = 0;
    uint32_t mask = 0;
    uint32_t ack = 0;
    bool ready = true;
    if (control & RESET_CTL_CAT_ERROR) {
        request = RESET_CTL_CAT_ERROR;
        mask = RESET_CTL_CAT_ERROR;
        ack = 0;
    } else if (!(control & RESET_CTL_READY_TO_RESET)) {
        request = RESET_CTL_REQUEST_RESET;
        mask = RESET_CTL_READY_TO_RESET;
        ack = RESET_CTL_READY_TO_RESET;
    }
    if (request != 0) {
        writeReg(base + RING_RESET_CTL, maskedEnable(request));
        ready = waitMs(700, [&] { return (readReg(base + RING_RESET_CTL) & mask) == ack; });
    }

    bool done = true;
    for (int loop = 0; loop < 2 && done; ++loop) {
        writeReg(REG_GDRST, e.resetDomain);
        done = waitMs(10, [&] { return (readReg(REG_GDRST) & e.resetDomain) == 0; });
    }
    waitMs(1, [] { return false; });   // let the engine settle (i915: udelay(50))
    writeReg(base + RING_RESET_CTL, maskedDisable(RESET_CTL_REQUEST_RESET));

    log("[i915] ");
    log(e.name);
    log(" reset: stopped=");
    log(idle ? "yes" : "no");
    log(" ready=");
    log(ready ? "yes" : "no");
    log(" done=");
    log(done ? "yes" : "no");
    log("\n");
    return done;
}

// The engine's buffers, made once and kept: its status page, a context, a
// ring and the page the commands write to. Then execlists are switched on
// (a reset turns them off).
bool IntelGen9Driver::setUpEngine(EngineState& e) {
    if (!e.statusPage) {
        e.statusPage = createBuffer(GTT_PAGE_SIZE, GTT_PAGE_SIZE, false);
    }
    if (!e.context) {
        e.context = createBuffer(e.contextBytes, GTT_PAGE_SIZE, false);
    }
    if (!e.ring) {
        e.ring = createBuffer(kRingBytes, GTT_PAGE_SIZE, false);
    }
    if (!e.target) {
        e.target = createBuffer(GTT_PAGE_SIZE, GTT_PAGE_SIZE, false);
    }
    if (!e.statusPage || !e.context || !e.ring || !e.target) {
        log("[i915] ");
        log(e.name);
        log(": could not create its buffers\n");
        return false;
    }

    // enable_execlists() and reset_csb_pointers().
    const uint32_t base = e.base;
    writeReg(base + RING_HWSTAM, 0xFFFFFFFFu);
    writeReg(base + RING_MODE_GEN7, maskedEnable(GFX_RUN_LIST_ENABLE));
    writeReg(base + RING_MI_MODE, maskedDisable(STOP_RING));
    writeReg(base + RING_HWS_PGA, static_cast<uint32_t>(e.statusPage->gttOffset));
    (void)readReg(base + RING_HWS_PGA);
    const uint32_t last = CSB_ENTRIES - 1;
    writeReg(base + RING_CONTEXT_STATUS_PTR, 0xFFFFu << 16 | last << 8 | last);
    (void)readReg(base + RING_CONTEXT_STATUS_PTR);
    e.csbRead = last;
    e.csbIdle = true;
    // The MOCS tables (i915's intel_mocs_init_engine, and the L3 table of
    // init_l3cc_table): what Mesa's MOCS indexes mean for caching. A reset
    // may clear them, so on every set-up; the render engine's are also in
    // its default context (they are context-saved).
    const uint32_t mocsBase = &e == &engines[0] ? GEN9_RCS_MOCS_BASE : GEN9_BCS_MOCS_BASE;
    for (uint32_t i = 0; i < MOCS_ENTRIES; ++i) {
        writeReg(mocsBase + i * 4, mocsControl(i));
    }
    for (uint32_t i = 0; i < MOCS_ENTRIES / 2; ++i) {
        writeReg(GEN9_LNCFCMOCS_BASE + i * 4, mocsL3Register(i));
    }
    if (!e.mocsLogged) {
        e.mocsLogged = true;
        uint32_t hold = 0;
        for (uint32_t i = 0; i < MOCS_ENTRIES; ++i) {
            hold += readReg(mocsBase + i * 4) == mocsControl(i);
        }
        uint32_t l3hold = 0;
        for (uint32_t i = 0; i < MOCS_ENTRIES / 2; ++i) {
            l3hold += readReg(GEN9_LNCFCMOCS_BASE + i * 4) == mocsL3Register(i);
        }
        log("[i915] ");
        log(e.name);
        log(": MOCS table ");
        logDec(hold);
        log(" of 62 hold, L3 ");
        logDec(l3hold);
        log(" of 31\n");
    }
    // The reset masked the engine's interrupts again (RING_IMR): unmask
    // what the requests wait for, as i915's reset path does.
    if (interruptsOn) {
        writeReg(base + RING_IMR, ~(kEngineInterrupts << e.irqShift));
        (void)readReg(base + RING_IMR);
    }
    e.userReady = true;
    if (renderingPrepared) {
        applyEngineWorkarounds(e);
    }
    return true;
}

uint64_t IntelGen9Driver::submit(EngineState& e, const uint32_t* dwords, uint32_t count, const Ppgtt* vm,
                                 bool verbose) {
    // The ring: the commands, padded with MI_NOOP to a whole qword (the tail
    // must be 8-byte aligned).
    uint32_t used = 0;
    for (; used < count && used * 4 < kRingBytes; ++used) {
        *bufferDword(e.ring, used * 4) = dwords[used];
    }
    while (used % 2 != 0) {
        *bufferDword(e.ring, used * 4) = MI_NOOP;
        ++used;
    }
    flushPages(e.ring);

    // A fresh context: an empty per-process status page, then the register
    // state with the ring in it. Without `vm` its page directories point at
    // the scratch page (nothing goes through a PPGTT); with it, the context
    // is a 64-bit one whose PDP0 holds vm's PML4.
    for (uint64_t offset = 0; offset < LRC_STATE_OFFSET; offset += 4) {
        *bufferDword(e.context, offset) = 0;
    }
    ContextSetup setup = {};
    setup.layout = e.layout;
    setup.engineBase = e.base;
    setup.ringGgtt = static_cast<uint32_t>(e.ring->gttOffset);
    setup.ringBytes = static_cast<uint32_t>(kRingBytes);
    setup.head = 0;
    setup.tail = used * 4;
    for (uint64_t& pdp : setup.pdp) {
        pdp = scratchPage;
    }
    setup.fourLevel = vm != nullptr;
    setup.pml4 = vm ? vm->pml4 : 0;
    auto* state = const_cast<uint32_t*>(bufferDword(e.context, LRC_STATE_OFFSET));
    buildContextState(state, setup);
    flushPages(e.context);

    // execlists_submit_ports(): element 1 (empty) then element 0, each upper
    // dword first. Logged before, so the log line is not part of a timing.
    // The descriptor's context id is 21 bits wide (i915's GEN8_CTX_ID_WIDTH).
    const uint32_t contextId = nextContextId;
    nextContextId = nextContextId % 0x1FFFFFu + 1;
    const uint64_t desc = contextDescriptor(static_cast<uint32_t>(e.context->gttOffset), contextId, vm != nullptr);
    if (verbose) {
        log("[i915] ");
        log(e.name);
        log(": submitting context ");
        logHex(desc);
        log(" ring ");
        logHex32(setup.ringGgtt);
        log(" tail ");
        logHex32(setup.tail);
        log("\n");
    }
    writeElsp(e, desc);
    return time_get_uptime_ms();
}

// execlists_submit_ports(): element 1 (empty) then element 0, each upper
// dword first.
void IntelGen9Driver::writeElsp(EngineState& e, uint64_t descriptor) {
    writeReg(e.base + RING_ELSP, 0);
    writeReg(e.base + RING_ELSP, 0);
    writeReg(e.base + RING_ELSP, static_cast<uint32_t>(descriptor >> 32));
    writeReg(e.base + RING_ELSP, static_cast<uint32_t>(descriptor));
}

void IntelGen9Driver::logEngineState(const EngineState& e) {
    const uint32_t base = e.base;
    log("[i915] ");
    log(e.name);
    logReg("HEAD", readReg(base + RING_HEAD));
    logReg("TAIL", readReg(base + RING_TAIL));
    logReg("START", readReg(base + RING_START));
    logReg("CTL", readReg(base + RING_CTL));
    logReg("ACTHD", readReg(base + RING_ACTHD));
    log("\n[i915]      ");
    logReg("MI_MODE", readReg(base + RING_MI_MODE));
    logReg("MODE", readReg(base + RING_MODE_GEN7));
    logReg("EXECLIST", readReg(base + RING_EXECLIST_STATUS_LO));
    logReg("/", readReg(base + RING_EXECLIST_STATUS_HI));
    logReg("EIR", readReg(base + RING_EIR));
    logReg("ESR", readReg(base + RING_ESR));
    log("\n[i915]      ");
    logReg("IPEHR", readReg(base + RING_IPEHR));
    logReg("INSTDONE", readReg(base + RING_INSTDONE));
    logReg("BBADDR", readReg(base + RING_BBADDR));
    logReg("IMR", readReg(base + RING_IMR));
    logReg("HWSTAM", readReg(base + RING_HWSTAM));
    log("\n[i915]      ");
    logReg("CSB_PTR", readReg(base + RING_CONTEXT_STATUS_PTR));
    for (uint32_t i = 0; i < CSB_ENTRIES; ++i) {
        logReg("csb", readReg(base + RING_EXECLISTS_STATUS_BUF + i * 8));
        logReg("", readReg(base + RING_EXECLISTS_STATUS_BUF + i * 8 + 4));
    }
    log("\n");
}

namespace {

// Two stores: MI_STORE_DWORD_IMM, then on the blitter MI_FLUSH_DW's
// post-sync write, on the render engine (which has no MI_FLUSH_DW) a second
// MI_STORE_DWORD_IMM.
uint32_t storeCommands(uint32_t* out, uint64_t target, bool blitter) {
    uint32_t n = 0;
    out[n++] = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
    out[n++] = static_cast<uint32_t>(target + kStoreOffset);
    out[n++] = 0;
    out[n++] = kStoreValue;
    if (blitter) {
        out[n++] = (MI_FLUSH_DW + 1) | MI_FLUSH_DW_OP_STOREDW;
        out[n++] = static_cast<uint32_t>(target + kSecondOffset) | MI_FLUSH_DW_USE_GTT;
    } else {
        out[n++] = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
        out[n++] = static_cast<uint32_t>(target + kSecondOffset);
    }
    out[n++] = 0;
    out[n++] = kSecondValue;
    return n;
}

} // namespace

// Resets the engine, runs the two stores and checks them.
bool IntelGen9Driver::runStores(EngineState& e) {
    if (!resetEngine(e) || !setUpEngine(e)) {
        return false;
    }
    writeBufferDword(e.target, kStoreOffset, 0);
    writeBufferDword(e.target, kSecondOffset, 0);
    uint32_t commands[16];
    const uint32_t count = storeCommands(commands, e.target->gttOffset, e.base == BLT_RING_BASE);
    const uint64_t start = submit(e, commands, count);
    const bool ok = waitMs(kRunMs, [&] {
        return readBufferDword(e.target, kStoreOffset) == kStoreValue &&
               readBufferDword(e.target, kSecondOffset) == kSecondValue;
    });
    log("[i915] ");
    log(e.name);
    log(" stores: ");
    logHex32(readBufferDword(e.target, kStoreOffset));
    log(", ");
    logHex32(readBufferDword(e.target, kSecondOffset));
    log(ok ? " (expected) after " : " (expected 0xc0ffee01, 0xc0ffee02) after ");
    logDec(time_get_uptime_ms() - start);
    log(" ms\n");
    logEngineState(e);
    return ok;
}

bool IntelGen9Driver::engineStoreTest(EngineId id) {
    if (!initializeGtt()) {
        return false;
    }
    if (!forcewakeGet()) {
        return false;
    }
    const bool ok = runStores(engine(id));
    forcewakePut();
    return ok;
}

bool IntelGen9Driver::engineHangTest() {
    if (!initializeGtt()) {
        return false;
    }
    if (!forcewakeGet()) {
        return false;
    }
    EngineState& e = engine(EngineId::Blitter);
    bool hung = false;
    bool recovered = false;
    if (resetEngine(e) && setUpEngine(e)) {
        // Wait until the semaphore dword is non-zero (it never is), then
        // store a marker that must therefore never appear.
        writeBufferDword(e.target, kSemaphoreOffset, 0);
        writeBufferDword(e.target, kStoreOffset, 0);
        const uint64_t target = e.target->gttOffset;
        const uint32_t commands[] = {
            MI_SEMAPHORE_WAIT | MI_SEMAPHORE_GLOBAL_GTT | MI_SEMAPHORE_POLL | MI_SEMAPHORE_SAD_NEQ_SDD,
            0,
            static_cast<uint32_t>(target + kSemaphoreOffset),
            0,
            MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT,
            static_cast<uint32_t>(target + kStoreOffset),
            0,
            kNeverValue,
        };
        submit(e, commands, sizeof(commands) / sizeof(commands[0]));
        const bool finished = waitMs(kHangMs, [&] { return readBufferDword(e.target, kStoreOffset) == kNeverValue; });
        hung = !finished;
        log(hung ? "[i915] blitter hang: still waiting on the semaphore after 300 ms (expected)\n"
                 : "[i915] blitter hang: the ring finished, so the semaphore did not hold\n");
        logEngineState(e);

        // Recover: reset the engine and run the stores again.
        recovered = runStores(e);
        log(recovered ? "[i915] blitter after the reset: stores ok\n"
                      : "[i915] blitter after the reset: the stores did not land\n");
    }
    forcewakePut();
    return hung && recovered;
}

namespace {

// Where the PPGTT test maps its pages: the batch near the top of the 48-bit
// space and the target low, so the walk goes through different tables at
// every level.
constexpr uint64_t kPpgttBatchVa = 0x00007FFFFFFF0000ULL;
constexpr uint64_t kPpgttTargetVa = 0x0000123456780000ULL;
constexpr uint32_t kPpgttValue = 0x0BADA550;
constexpr uint32_t kPpgttSecond = 0x0BADA551;
constexpr uint32_t kPpgttDone = 0xD0D0D0D0;

} // namespace

// A batch in a fresh PPGTT stores two values into a buffer mapped there; the
// ring then stores a marker through the GGTT. The CPU writes the batch and
// reads the stores through its write-back direct map without flushing a
// line: coherence through the LLC (PPAT index 4 for both).
bool IntelGen9Driver::runPpgttStores(EngineState& e) {
    if (!resetEngine(e) || !setUpEngine(e)) {
        return false;
    }
    Ppgtt* vm = createPpgtt();
    GpuBuffer* batch = createBuffer(GTT_PAGE_SIZE, GTT_PAGE_SIZE, false);
    GpuBuffer* target = createBuffer(GTT_PAGE_SIZE, GTT_PAGE_SIZE, false);
    bool ok = vm && batch && target && ppgttMap(vm, kPpgttBatchVa, batch, true) &&
              ppgttMap(vm, kPpgttTargetVa, target, true);
    if (!ok) {
        log("[i915] ppgtt: could not build the address space\n");
    } else {
        // Written and read through the CPU's write-back mapping, no clflush.
        auto* batchWords = reinterpret_cast<volatile uint32_t*>(VMM::PhysToVirt(batch->pages[0]));
        auto* targetWords = reinterpret_cast<volatile uint32_t*>(VMM::PhysToVirt(target->pages[0]));
        targetWords[0] = 0;
        targetWords[1] = 0;
        uint32_t n = 0;
        const uint32_t batchCommands[] = {
            MI_STORE_DWORD_IMM_GEN4, static_cast<uint32_t>(kPpgttTargetVa), static_cast<uint32_t>(kPpgttTargetVa >> 32),
            kPpgttValue,
            MI_STORE_DWORD_IMM_GEN4, static_cast<uint32_t>(kPpgttTargetVa + 4),
            static_cast<uint32_t>(kPpgttTargetVa >> 32), kPpgttSecond,
            MI_BATCH_BUFFER_END, MI_NOOP,
        };
        for (const uint32_t word : batchCommands) {
            batchWords[n++] = word;
        }
        writeBufferDword(e.target, kStoreOffset, 0);
        const uint32_t ring[] = {
            MI_BATCH_BUFFER_START_GEN8 | MI_BATCH_PPGTT, static_cast<uint32_t>(kPpgttBatchVa),
            static_cast<uint32_t>(kPpgttBatchVa >> 32),
            MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT, static_cast<uint32_t>(e.target->gttOffset + kStoreOffset), 0,
            kPpgttDone,
        };
        const uint64_t start = submit(e, ring, sizeof(ring) / sizeof(ring[0]), vm);
        const bool done = waitMs(kRunMs, [&] { return readBufferDword(e.target, kStoreOffset) == kPpgttDone; });
        const uint32_t first = targetWords[0];
        const uint32_t second = targetWords[1];
        log("[i915] ");
        log(e.name);
        log(" ppgtt: batch at ");
        logHex(kPpgttBatchVa);
        log(done ? " ran" : " did NOT finish");
        log(" in ");
        logDec(time_get_uptime_ms() - start);
        log(" ms; read back without a flush: ");
        logHex32(first);
        log(", ");
        logHex32(second);
        ok = done && first == kPpgttValue && second == kPpgttSecond;
        if (!ok) {
            flushLine(&targetWords[0]);
            log("; after a clflush: ");
            logHex32(targetWords[0]);
            log(", ");
            logHex32(targetWords[1]);
        }
        log(ok ? " (expected)\n" : " (expected 0x0bada550, 0x0bada551)\n");
        log("[i915] ppgtt: ");
        logDec(vm->tablePages);
        log(" table pages for two mappings\n");
        if (!ok) {
            logEngineState(e);
        }
    }
    if (vm) {
        destroyPpgtt(vm);
    }
    destroyBuffer(batch);
    destroyBuffer(target);
    return ok;
}

bool IntelGen9Driver::ppgttTest() {
    if (!initializeGtt() || !forcewakeGet()) {
        return false;
    }
    programPrivatePat();
    const bool render = runPpgttStores(engine(EngineId::Render));
    const bool blitter = runPpgttStores(engine(EngineId::Blitter));
    forcewakePut();
    return render && blitter;
}

// gen8_gt_irq_postinstall for the render and blitter engines: their user
// interrupt and command-streamer error in GT bank 0, unmasked in each
// engine's RING_IMR too; then the master. The MSI is registered on first use.
bool IntelGen9Driver::enableInterrupts() {
    if (!msiRegistered) {
        if (!PCI::get().registerMSIInterrupt(0, bus, device, function, &gpuInterrupt(), &msiVector)) {
            log("[i915] interrupts: MSI registration failed\n");
            return false;
        }
        msiRegistered = true;
    }
    writeReg(REG_MASTER_IRQ, 0);
    writeReg(REG_GT_IMR0, 0xFFFFFFFFu);
    writeReg(REG_GT_IER0, 0);
    writeReg(REG_GT_IIR0, 0xFFFFFFFFu);
    writeReg(REG_GT_IIR0, 0xFFFFFFFFu);
    const uint32_t enabled = kEngineInterrupts << RCS_IRQ_SHIFT | kEngineInterrupts << BCS_IRQ_SHIFT;
    writeReg(REG_GT_IER0, enabled);
    writeReg(REG_GT_IMR0, ~enabled);
    (void)readReg(REG_GT_IMR0);
    // RING_IMR takes the engine's bits where GT bank 0 has them (i915 writes
    // ~(irq_enable_mask | irq_keep_mask), both shifted): the blitter's user
    // interrupt is bit 16 there too.
    for (EngineState& e : engines) {
        writeReg(e.base + RING_IMR, ~(kEngineInterrupts << e.irqShift));
    }
    foreignInterrupts = 0;
    log("[i915] interrupts: MSI vector ");
    logDec(msiVector);
    log(" to cpu ");
    logDec(static_cast<uint64_t>(msiTargetCpu(bus, device, function)));
    logReg("GT_IER0", readReg(REG_GT_IER0));
    logReg("PCU_IER", readReg(REG_PCU_IER));
    log("\n");
    writeReg(REG_MASTER_IRQ, MASTER_IRQ_CONTROL);
    interruptsOn = true;
    return true;
}

void IntelGen9Driver::disableInterrupts() {
    interruptsOn = false;
    writeReg(REG_MASTER_IRQ, 0);
    writeReg(REG_GT_IER0, 0);
    writeReg(REG_GT_IMR0, 0xFFFFFFFFu);
    for (EngineState& e : engines) {
        writeReg(e.base + RING_IMR, 0xFFFFFFFFu);
    }
}

// gen8_irq_handler: master off, read which banks fired, acknowledge GT bank
// 0, master back on. Runs under the big kernel lock on whichever CPU the MSI
// reaches.
void IntelGen9Driver::handleInterrupt() {
    writeReg(REG_MASTER_IRQ, 0);
    const uint32_t master = readReg(REG_MASTER_IRQ);
    if (master & (MASTER_GT_RCS_IRQ | MASTER_GT_BCS_IRQ)) {
        const uint32_t iir = readReg(REG_GT_IIR0);
        if (iir != 0) {
            lastIir = iir;
            for (int i = 0; i < kEngineCount; ++i) {
                if ((iir >> engines[i].irqShift) & GT_RENDER_USER_INTERRUPT) {
                    userInterrupts[i] = userInterrupts[i] + 1;
                    lastInterruptMs[i] = time_get_uptime_ms();
                }
                if ((iir >> engines[i].irqShift) & GT_CONTEXT_SWITCH_INTERRUPT) {
                    contextSwitches[i] = contextSwitches[i] + 1;
                }
            }
            writeReg(REG_GT_IIR0, iir);
        }
    }
    interruptCount = interruptCount + 1;
    lastInterruptCpu = currentCpuIndex();
    if (master & ~(MASTER_GT_RCS_IRQ | MASTER_GT_BCS_IRQ | MASTER_IRQ_CONTROL)) {
        foreignInterrupts = foreignInterrupts + 1;
    }
    if (foreignInterrupts < kMaxForeignInterrupts) {
        writeReg(REG_MASTER_IRQ, MASTER_IRQ_CONTROL);
    }
    inInterrupt = true;
    processRequests();
    inInterrupt = false;
    Scheduler::get().wakeAllBlockedProcesses();
}

// Like a blocking socket call: sleeps in short steps, which hands the big
// kernel lock to others so the interrupt handler can run and wake us.
bool IntelGen9Driver::sleepUntilChanged(volatile uint64_t& counter, uint64_t from, uint64_t ms) {
    Process* current = Scheduler::get().getCurrentProcess();
    const uint64_t deadline = time_get_uptime_ms() + ms;
    while (counter == from) {
        const uint64_t now = time_get_uptime_ms();
        if (now >= deadline || !current) {
            return counter != from;
        }
        current->sleepUntil(now + 10 < deadline ? now + 10 : deadline, true);
        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
        current->clearSleep();
    }
    return true;
}

bool IntelGen9Driver::interruptTest() {
    if (!initializeGtt()) {
        return false;
    }
    if (!forcewakeGet()) {
        return false;
    }
    bool ok = true;
    for (int i = 0; i < kEngineCount && ok; ++i) {
        EngineState& e = engines[i];
        if (!resetEngine(e) || !setUpEngine(e)) {
            ok = false;
            break;
        }
    }
    if (ok) {
        ok = enableInterrupts();
    }
    for (int i = 0; i < kEngineCount && ok; ++i) {
        EngineState& e = engines[i];
        writeBufferDword(e.target, kInterruptOffset, 0);
        const uint32_t commands[] = {
            MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT,
            static_cast<uint32_t>(e.target->gttOffset + kInterruptOffset),
            0,
            kInterruptValue,
            MI_USER_INTERRUPT,
        };
        const uint64_t before = userInterrupts[i];
        const uint64_t start = submit(e, commands, sizeof(commands) / sizeof(commands[0]));
        const bool woke = sleepUntilChanged(userInterrupts[i], before, kRunMs);
        const uint64_t elapsed = time_get_uptime_ms() - start;
        const uint32_t value = readBufferDword(e.target, kInterruptOffset);
        log("[i915] ");
        log(e.name);
        if (woke) {
            log(" user interrupt arrived after ");
            logDec(lastInterruptMs[i] >= start ? lastInterruptMs[i] - start : 0);
            log(" ms, woke the caller after ");
        } else {
            log(" no user interrupt within ");
        }
        logDec(elapsed);
        log(" ms; IIR was ");
        logHex32(lastIir);
        log(", store ");
        logHex32(value);
        log(value == kInterruptValue ? " (expected)\n" : " (expected 0x1a7e0001)\n");
        if (!woke) {
            logEngineState(e);
            logReg("GT_IIR0", readReg(REG_GT_IIR0));
            logReg("GT_IMR0", readReg(REG_GT_IMR0));
            logReg("RING_IMR", readReg(e.base + RING_IMR));
            logReg("MASTER", readReg(REG_MASTER_IRQ));
            log("\n");
            logLapicOf(msiTargetCpu(bus, device, function));
        }
        ok = woke && value == kInterruptValue;
    }
    log("[i915] interrupts taken: ");
    logDec(interruptCount);
    log(" (the last on cpu ");
    logDec(lastInterruptCpu);
    log("), from other sources: ");
    logDec(foreignInterrupts);
    log("\n");
    disableInterrupts();
    forcewakePut();
    return ok;
}

namespace {

// Sleeps about `ms` (the big kernel lock released, as for any blocking call).
void sleepMs(uint64_t ms) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return;
    }
    current->sleepUntil(time_get_uptime_ms() + ms, true);
    current->setState(ProcessState::Blocked);
    Scheduler::get().scheduleFromSyscall();
    current->clearSleep();
}

// Where a request's breadcrumb stores its sequence number in the engine's target page.
// (MI_FLUSH_DW's address must have bit 5 clear, and both stores are qwords.)
constexpr uint64_t kUserSeqnoOffset = 64;

} // namespace

bool IntelGen9Driver::claimEngines(uint64_t waitMs) {
    const uint64_t deadline = time_get_uptime_ms() + waitMs;
    for (;;) {
        processRequests();
        if (!enginesClaimed && queuedCount == 0) {
            break;
        }
        if (time_get_uptime_ms() >= deadline) {
            return false;
        }
        sleepMs(1);
    }
    enginesClaimed = true;
    return true;
}

void IntelGen9Driver::releaseEngines() {
    // An experiment may have left its contexts and status events on the
    // engines: reset and set them up again before the next request.
    for (EngineState& e : engines) {
        e.userReady = false;
    }
    enginesClaimed = false;
    processRequests();   // what was queued meanwhile
}

void IntelGen9Driver::sleepForGpu(uint64_t seen, uint64_t ms) {
    sleepUntilChanged(interruptCount, seen, ms);
}

// A context's ring: 16 KiB, as i915 gives a user context.
constexpr uint64_t kContextRingBytes = 4 * GTT_PAGE_SIZE;

static void flushRange(GpuBuffer* buffer, uint64_t offset, uint64_t bytes) {
    for (uint64_t at = offset & ~63ull; at < offset + bytes; at += 64) {
        asm volatile("clflush (%0)" : : "r"(bufferDword(buffer, at)) : "memory");
    }
    asm volatile("mfence" ::: "memory");
}

// i915's __engines_record_defaults: on a clean engine (just reset, its
// registers at their reset values), a scratch context runs with restore
// inhibited; on the render engine it sets the context workarounds (read
// back once, to check them) and runs the null-state batch. A second,
// throwaway context then takes the engine, which makes it save the scratch
// one, and that saved image is kept: every new context starts as a copy, so
// none inherits another's registers (restore-inhibited, it would keep
// whatever the engine held) and all begin from the same defined state.
bool IntelGen9Driver::recordDefaultContext(EngineId id) {
    EngineState& e = engine(id);
    if (e.defaultImage) {
        return true;
    }
    const bool render = id == EngineId::Render;
    if (!resetEngine(e) || !setUpEngine(e)) {
        return false;
    }
    HwContext scratch;
    Ppgtt* vm = createPpgtt();
    bool ok = vm && createHwContext(id, scratch);
    if (ok) {
        if (++userSeqno == 0) {
            userSeqno = 1;
        }
        const uint32_t seqno = userSeqno;
        uint32_t commands[400];
        uint32_t* cs = commands;
        if (render) {
            cs = emitFlushRcs(cs, true, true);
            cs = emitMocsTables(cs, GEN9_RCS_MOCS_BASE);
            cs = emitContextWorkarounds(cs, contextWorkarounds, contextWorkaroundCount);
            cs = emitFlushRcs(cs, true, true);
            for (uint32_t i = 0; i < contextWorkaroundCount; ++i) {
                cs = emitStoreRegisterGgtt(cs, contextWorkarounds[i].reg,
                                           static_cast<uint32_t>(workaroundCheck->gttOffset + i * 4));
            }
            cs = emitBatchStart(cs, nullState->gttOffset, false);
        } else {
            cs = emitFlushXcs(cs, true);
        }
        cs = emitBreadcrumb(cs, render, seqno, static_cast<uint32_t>(e.target->gttOffset + kUserSeqnoOffset));
        if ((cs - commands) % 2 != 0) {
            *cs++ = MI_NOOP;
        }
        const uint32_t bytes = static_cast<uint32_t>(cs - commands) * 4;
        for (uint32_t i = 0; i < bytes / 4; ++i) {
            *bufferDword(scratch.ring, i * 4) = commands[i];
        }
        flushRange(scratch.ring, 0, bytes);
        ContextSetup setup = {};
        setup.layout = e.layout;
        setup.engineBase = e.base;
        setup.ringGgtt = static_cast<uint32_t>(scratch.ring->gttOffset);
        setup.ringBytes = static_cast<uint32_t>(kContextRingBytes);
        setup.head = 0;
        setup.tail = bytes;
        setup.fourLevel = true;
        setup.pml4 = vm->pml4;
        setup.rpcs = render ? rpcs : 0;
        for (uint64_t offset = 0; offset < LRC_STATE_OFFSET; offset += 4) {
            *bufferDword(scratch.image, offset) = 0;
        }
        buildContextState(const_cast<uint32_t*>(bufferDword(scratch.image, LRC_STATE_OFFSET)), setup);
        flushRange(scratch.image, 0, LRC_STATE_OFFSET + GTT_PAGE_SIZE);
        writeBufferDword(e.target, kUserSeqnoOffset, 0);
        writeElsp(e, contextDescriptor(static_cast<uint32_t>(scratch.image->gttOffset), scratch.hwId, true));
        ok = waitMs(kRunMs, [&] { return readBufferDword(e.target, kUserSeqnoOffset) == seqno; });
        if (!ok) {
            log("[i915] ");
            log(e.name);
            log(": the default context did not finish\n");
            logEngineState(e);
        }
    }
    if (ok) {
        // Switch to a throwaway context so the engine saves the scratch one.
        writeBufferDword(e.target, kStoreOffset, 0);
        const uint32_t store[] = {
            MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT, static_cast<uint32_t>(e.target->gttOffset + kStoreOffset), 0,
            kStoreValue,
        };
        submit(e, store, sizeof(store) / sizeof(store[0]), nullptr, false);
        ok = waitMs(kRunMs, [&] { return readBufferDword(e.target, kStoreOffset) == kStoreValue; }) &&
             waitEngineIdle(e, kRunMs);
        if (!ok) {
            log("[i915] ");
            log(e.name);
            log(": the throwaway context did not finish\n");
            logEngineState(e);
        }
    }
    if (ok) {
        e.defaultImage = static_cast<uint8_t*>(kmalloc(e.contextBytes));
        ok = e.defaultImage != nullptr;
    }
    if (ok) {
        for (uint32_t page = 0; page < scratch.image->pageCount; ++page) {
            auto* source = reinterpret_cast<uint8_t*>(VMM::PhysToVirt(scratch.image->pages[page]));
            for (uint64_t line = 0; line < GTT_PAGE_SIZE; line += 64) {
                asm volatile("clflush (%0)" : : "r"(source + line) : "memory");
            }
            asm volatile("mfence" ::: "memory");
            memcpy(e.defaultImage + static_cast<uint64_t>(page) * GTT_PAGE_SIZE, source, GTT_PAGE_SIZE);
        }
        if (render && !contextWorkaroundsChecked) {
            contextWorkaroundsChecked = true;
            verifyContextWorkarounds();
        }
        log("[i915] ");
        log(e.name);
        log(": default context recorded (");
        logDec(e.contextBytes / 1024);
        log(render ? " KiB, after the null-state batch)\n" : " KiB)\n");
    }
    if (scratch.image || scratch.ring) {
        destroyHwContext(scratch);
    }
    if (vm) {
        destroyPpgtt(vm);
    }
    return ok;
}

bool IntelGen9Driver::createHwContext(EngineId id, HwContext& context) {
    if (!initializeGtt()) {
        return false;
    }
    EngineState& e = engine(id);
    context.image = createBuffer(e.contextBytes, GTT_PAGE_SIZE, false);
    context.ring = createBuffer(kContextRingBytes, GTT_PAGE_SIZE, false);
    if (!context.image || !context.ring) {
        destroyHwContext(context);
        return false;
    }
    context.tail = 0;
    context.started = false;
    context.hwId = nextContextId;
    nextContextId = nextContextId % 0x1FFFFFu + 1;
    return true;
}

void IntelGen9Driver::destroyHwContext(HwContext& context) {
    if (context.image) {
        destroyBuffer(context.image);
    }
    if (context.ring) {
        destroyBuffer(context.ring);
    }
    context = HwContext{};
}

// What an engine needs before user work, done while it is idle (the first
// time, and again after gpu-test's experiments reset it): the rendering
// setup, a reset and set-up engine with its workarounds, its default
// context, and the interrupts the requests' waiters sleep on.
bool IntelGen9Driver::prepareEngine(EngineState& e, EngineId id) {
    if (e.userReady && e.workaroundsApplied && e.defaultImage && interruptsOn) {
        return true;
    }
    if (e.active || !forcewakeGet()) {
        return e.active != nullptr;   // busy means it is prepared
    }
    bool ok = prepareRendering() && (e.userReady || (resetEngine(e) && setUpEngine(e)));
    if (ok && !e.workaroundsApplied) {
        applyEngineWorkarounds(e);
    }
    ok = ok && (e.defaultImage || recordDefaultContext(id));
    if (ok && !interruptsOn) {
        ok = enableInterrupts();
    }
    forcewakePut();
    return ok;
}

int IntelGen9Driver::queueRequest(Request* request) {
    if (!request || !request->vm || !request->context || !request->context->image || !initializeGtt()) {
        return -19;   // ENODEV
    }
    EngineState& e = engine(request->engine);
    // Not while gpu-test holds the engines: its experiment may be using
    // this one (the request is prepared for when it is dispatched).
    if (!enginesClaimed && !prepareEngine(e, request->engine)) {
        return -5;    // EIO
    }
    request->hung = false;
    request->next = nullptr;
    request->queuedNs = time_get_uptime_ns();
    if (e.queueTail) {
        e.queueTail->next = request;
    } else {
        e.queueHead = request;
    }
    e.queueTail = request;
    ++queuedCount;
    processRequests();
    return 0;
}

// One request on the engine, as i915 makes it for an execbuffer: an
// invalidation of the TLBs and read caches (the address space may have
// changed since the context last ran), the batch (non-privileged, from the
// PPGTT), and the breadcrumb: a flush and a GGTT store of a fresh sequence
// number once everything is done. The commands go into the context's own
// ring after what it ran before; a context's first run starts from a copy
// of the engine's default image, later ones from what the engine saved.
void IntelGen9Driver::dispatch(EngineState& e, Request* request) {
    HwContext& context = *request->context;
    const bool render = &e == &engines[0];
    const bool first = !context.started;
    if (++userSeqno == 0) {
        userSeqno = 1;
    }
    request->seqno = userSeqno;

    uint32_t commands[64];
    uint32_t* cs = commands;
    cs = render ? emitFlushRcs(cs, false, true) : emitFlushXcs(cs, true);
    cs = emitBatchStart(cs, request->batchVa, true);
    cs = emitBreadcrumb(cs, render, request->seqno, static_cast<uint32_t>(e.target->gttOffset + kUserSeqnoOffset));
    if ((cs - commands) % 2 != 0) {
        *cs++ = MI_NOOP;   // the tail must be qword aligned
    }
    const uint32_t bytes = static_cast<uint32_t>(cs - commands) * 4;

    const uint32_t ringBytes = static_cast<uint32_t>(kContextRingBytes);
    const uint32_t head = context.tail;
    const uint32_t at = ringPlacement(head, bytes, ringBytes);
    if (at != head) {
        for (uint32_t offset = head; offset < ringBytes; offset += 4) {
            *bufferDword(context.ring, offset) = MI_NOOP;
        }
        flushRange(context.ring, head, ringBytes - head);
    }
    for (uint32_t i = 0; i < bytes / 4; ++i) {
        *bufferDword(context.ring, at + i * 4) = commands[i];
    }
    flushRange(context.ring, at, bytes);
    const uint32_t tail = (at + bytes) % ringBytes;

    auto* regs = const_cast<uint32_t*>(bufferDword(context.image, LRC_STATE_OFFSET));
    if (first) {
        for (uint32_t page = 0; page < context.image->pageCount; ++page) {
            memcpy(reinterpret_cast<void*>(VMM::PhysToVirt(context.image->pages[page])),
                   e.defaultImage + static_cast<uint64_t>(page) * GTT_PAGE_SIZE, GTT_PAGE_SIZE);
        }
        ContextSetup setup = {};
        setup.layout = e.layout;
        setup.engineBase = e.base;
        setup.ringGgtt = static_cast<uint32_t>(context.ring->gttOffset);
        setup.ringBytes = ringBytes;
        setup.head = head;
        setup.tail = tail;
        setup.fourLevel = true;
        setup.pml4 = request->vm->pml4;
        setup.rpcs = render ? rpcs : 0;
        buildContextState(regs, setup, true);
        if (render && indirectCtx) {
            setIndirectCtx(regs, static_cast<uint32_t>(indirectCtx->gttOffset), indirectCtxBytes);
        }
        flushRange(context.image, 0, static_cast<uint64_t>(context.image->pageCount) * GTT_PAGE_SIZE);
    } else {
        prepareContextResubmit(regs, head, tail);
        if (render && indirectCtx) {
            setIndirectCtx(regs, static_cast<uint32_t>(indirectCtx->gttOffset), indirectCtxBytes);
        }
        flushRange(context.image, LRC_STATE_OFFSET, GTT_PAGE_SIZE);
    }
    // Where the ring stands once this request is through; a context whose
    // request hangs loses its state instead (finishActive).
    context.tail = tail;
    context.started = true;

    writeBufferDword(e.target, kUserSeqnoOffset, 0);
    // Events of what ran before are not this request's.
    e.idleEvents += drainCsb(e);
    request->idleBefore = e.idleEvents;
    request->startedNs = time_get_uptime_ns();
    const uint64_t waited = request->startedNs - request->queuedNs;
    if (waited > stats.waitNsMax) {
        stats.waitNsMax = waited;
    }
    ++stats.dispatched;
    e.active = request;
    writeElsp(e, contextDescriptor(static_cast<uint32_t>(context.image->gttOffset), context.hwId, true));
}

// Done means the breadcrumb landed and the engine has saved the context:
// the store comes before the save, and the context's next request changes
// the image. The context status buffer reports the switch to idle once it
// is saved.
bool IntelGen9Driver::activeDone(EngineState& e) {
    Request* request = e.active;
    e.idleEvents += drainCsb(e);
    return e.idleEvents > request->idleBefore && readBufferDword(e.target, kUserSeqnoOffset) == request->seqno;
}

uint32_t IntelGen9Driver::drainCsb(EngineState& e) {
    const uint32_t write = readReg(e.base + RING_CONTEXT_STATUS_PTR) & 0x7;
    if (write >= CSB_ENTRIES) {
        return 0;   // reads 7 after a reset until the first event (then entry 0)
    }
    uint32_t idles = 0;
    while (e.csbRead != write) {
        e.csbRead = (e.csbRead + 1) % CSB_ENTRIES;
        const uint32_t status = readReg(e.base + RING_EXECLISTS_STATUS_BUF + e.csbRead * 8);
        e.csbIdle = (status & GEN8_CTX_STATUS_ACTIVE_IDLE) != 0;
        if (e.csbIdle) {
            ++idles;
        }
    }
    // The read pointer (bits 10:8), so the hardware knows the entries are consumed.
    writeReg(e.base + RING_CONTEXT_STATUS_PTR, maskedField(0x7u << 8, e.csbRead << 8));
    return idles;
}

bool IntelGen9Driver::waitEngineIdle(EngineState& e, uint64_t ms) {
    return waitMs(ms, [&] {
        e.idleEvents += drainCsb(e);
        return e.csbIdle && (readReg(e.base + RING_CONTEXT_STATUS_PTR) & 0x7) < CSB_ENTRIES;
    });
}

void IntelGen9Driver::finishActive(EngineState& e, int status) {
    Request* request = e.active;
    e.active = nullptr;
    --queuedCount;
    const uint64_t ran = time_get_uptime_ns() - request->startedNs;
    stats.runNsTotal += ran;
    if (ran > stats.runNsMax) {
        stats.runNsMax = ran;
    }
    if (status != 0) {
        ++stats.failed;
    } else if (inInterrupt) {
        ++stats.retiredByInterrupt;
    } else {
        ++stats.retiredByWaiter;
    }
    request->done->signaled = true;
    request->done->status = status;
    request->retire(request);
}

void IntelGen9Driver::processRequests() {
    if (processing || !gttInitialized) {
        return;
    }
    processing = true;
    bool progress = true;
    while (progress) {
        progress = false;
        for (int i = 0; i < kEngineCount; ++i) {
            EngineState& e = engines[i];
            if (e.active) {
                if (activeDone(e)) {
                    finishActive(e, 0);
                    progress = true;
                } else if (time_get_uptime_ns() - e.active->startedNs > kHangMs * 1000000ull) {
                    // A hang: the engine is reset, the request fails, and its
                    // context's state is lost (it starts afresh next time).
                    Request* request = e.active;
                    log("[drm] ");
                    log(e.name);
                    log(": the batch at ");
                    logHex(request->batchVa);
                    log(" made no progress in ");
                    logDec(kHangMs);
                    log(readBufferDword(e.target, kUserSeqnoOffset) == request->seqno
                            ? " ms (its breadcrumb landed, the switch-out was not reported)"
                            : " ms");
                    log("; resetting the engine, the request fails with EIO\n");
                    logActiveRequest(e);
                    logEngineState(e);
                    resetEngine(e);
                    setUpEngine(e);
                    request->context->started = false;
                    request->context->tail = 0;
                    request->hung = true;
                    finishActive(e, -5);
                    progress = true;
                }
            }
            if (e.active || !e.queueHead || enginesClaimed) {
                continue;
            }
            Request* next = e.queueHead;
            int failed = 0;
            bool ready = true;
            for (uint32_t d = 0; d < next->depCount; ++d) {
                if (!next->deps[d]->poll()) {
                    ready = false;
                } else if (next->deps[d]->status < 0) {
                    failed = next->deps[d]->status;
                }
            }
            if (!ready) {
                continue;   // in order: the rest of this engine's queue waits too
            }
            e.queueHead = next->next;
            if (!e.queueHead) {
                e.queueTail = nullptr;
            }
            if (failed == 0 && next->skip && *next->skip) {
                failed = -5;
            }
            if (failed != 0) {
                // Not run (i915 skips a request whose context is banned or
                // whose dependency failed, with the error).
                --queuedCount;
                ++stats.skipped;
                next->done->signaled = true;
                next->done->status = failed;
                next->retire(next);
                progress = true;
                continue;
            }
            // Forcewake is held from the first dispatch until both engines
            // are idle (put at the end of this function).
            if (!requestForcewake && forcewakeGet()) {
                requestForcewake = true;
            }
            if (!requestForcewake || !prepareEngine(e, i == 0 ? EngineId::Render : EngineId::Blitter)) {
                // The engine cannot be woken or set up: fail the request
                // rather than leave it waiting forever.
                log("[drm] ");
                log(e.name);
                log(": the engine could not be woken or set up; the request fails with EIO\n");
                --queuedCount;
                ++stats.failed;
                next->done->signaled = true;
                next->done->status = -5;
                next->retire(next);
                progress = true;
                continue;
            }
            dispatch(e, next);
            progress = true;
        }
    }
    if (requestForcewake && !engines[0].active && !engines[1].active) {
        forcewakePut();
        requestForcewake = false;
    }
    processing = false;
}

// The hung request as the driver sees it: its context's ring around the
// engine's head, the ring window in its image, and its breadcrumb.
void IntelGen9Driver::logActiveRequest(EngineState& e) {
    const Request* request = e.active;
    const HwContext& context = *request->context;
    log("[drm]       seqno ");
    logDec(request->seqno);
    log(", breadcrumb holds ");
    logDec(readBufferDword(e.target, kUserSeqnoOffset));
    log(", ring ");
    logHex32(static_cast<uint32_t>(context.ring->gttOffset));
    log(", image head ");
    logHex32(readBufferDword(context.image, LRC_STATE_OFFSET + CTX_RING_HEAD * 4));
    log(" tail ");
    logHex32(readBufferDword(context.image, LRC_STATE_OFFSET + CTX_RING_TAIL * 4));
    log(", idle events ");
    logDec(e.idleEvents);
    log(" (");
    logDec(request->idleBefore);
    log(" at dispatch), csb read ");
    logDec(e.csbRead);
    log(", running ");
    logDec((time_get_uptime_ns() - request->startedNs) / 1000000);
    log(" ms\n[drm]       ring:");
    const uint32_t head = readReg(e.base + RING_HEAD) & 0xFFFFCu;
    const uint32_t from = head >= 0x20 ? head - 0x20 : 0;
    for (uint32_t offset = from; offset < from + 0x40 && offset < kContextRingBytes; offset += 4) {
        log(offset == head ? " >" : " ");
        logHex32(readBufferDword(context.ring, offset));
    }
    log("\n");
}

// From the boot CPU's timer tick: a request that has run past kHangMs is
// dealt with even when nobody waits on the GPU or submits to it (a program
// blocked in poll() on a sync_file, or one that just went away). Only then:
// the interrupt retires everything else.
void IntelGen9Driver::timerTick() {
    if (queuedCount == 0 || processing || enginesClaimed) {
        return;
    }
    const uint64_t now = time_get_uptime_ns();
    bool overdue = false;
    for (const EngineState& e : engines) {
        overdue = overdue || (e.active && now - e.active->startedNs > kHangMs * 1000000ull);
    }
    if (!overdue) {
        return;
    }
    processRequests();
    Scheduler::get().wakeAllBlockedProcesses();
}

// i915's REG_READ of RING_TIMESTAMP with I915_REG_READ_8B_WA (read64_2x32):
// upper, lower, upper again until the upper half holds still.
bool IntelGen9Driver::readRenderTimestamp(uint64_t* value) {
    constexpr uint32_t RING_TIMESTAMP = 0x358;
    constexpr uint32_t RING_TIMESTAMP_UDW = 0x35c;
    if (!initialized || !forcewakeGet()) {
        return false;
    }
    const uint32_t base = engines[0].base;
    uint32_t upper = readReg(base + RING_TIMESTAMP_UDW);
    uint32_t lower = 0;
    for (int loop = 0; loop < 3; ++loop) {
        const uint32_t before = upper;
        lower = readReg(base + RING_TIMESTAMP);
        upper = readReg(base + RING_TIMESTAMP_UDW);
        if (upper == before) {
            break;
        }
    }
    forcewakePut();
    *value = static_cast<uint64_t>(upper) << 32 | lower;
    return true;
}

void IntelGen9Driver::logRequestState() {
    log("[drm] requests: ");
    logDec(stats.dispatched);
    log(" run, ");
    logDec(stats.retiredByInterrupt);
    log(" retired from the interrupt, ");
    logDec(stats.retiredByWaiter);
    log(" by a waiter, ");
    logDec(stats.failed);
    log(" failed, ");
    logDec(stats.skipped);
    log(" skipped; queued now ");
    logDec(queuedCount);
    log("\n[drm] time on the engine: ");
    const uint64_t ended = stats.retiredByInterrupt + stats.retiredByWaiter + stats.failed;
    logDec(ended ? stats.runNsTotal / ended / 1000 : 0);
    log(" us on average, ");
    logDec(stats.runNsMax / 1000);
    log(" us at most; longest wait in the queue ");
    logDec(stats.waitNsMax / 1000);
    log(" us\n[drm] forcewake: ");
    logDec(stats.forcewakeWakes);
    log(" wakes, ");
    logDec(stats.forcewakeWakes ? stats.forcewakeNsTotal / stats.forcewakeWakes / 1000 : 0);
    log(" us on average, ");
    logDec(stats.forcewakeNsMax / 1000);
    log(" us at most; held ");
    logDec(forcewakeCount);
    log(" times now\n[drm] interrupts: ");
    logDec(interruptCount);
    log(" (");
    for (int i = 0; i < kEngineCount; ++i) {
        log(engines[i].name);
        log(" ");
        logDec(contextSwitches[i]);
        log(" context switches, ");
        logDec(userInterrupts[i]);
        log(" user; ");
    }
    log("other sources ");
    logDec(foreignInterrupts);
    log("), ");
    log(interruptsOn ? "on" : "off");
    log("\n");
    if (!gttInitialized || !forcewakeGet()) {
        return;
    }
    for (EngineState& e : engines) {
        log("[drm] ");
        log(e.name);
        log(e.userReady ? ": set up" : ": not set up");
        log(e.active ? ", a request on it" : ", idle");
        uint32_t waiting = 0;
        for (Request* r = e.queueHead; r; r = r->next) {
            ++waiting;
        }
        log(", ");
        logDec(waiting);
        log(" waiting; idle events ");
        logDec(e.idleEvents);
        log(", csb read ");
        logDec(e.csbRead);
        log("\n");
        if (e.active) {
            logActiveRequest(e);
        }
        logEngineState(e);
    }
    forcewakePut();
}

} // namespace intel_gen9
