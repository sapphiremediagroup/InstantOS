// What the render engine needs before real work (see intel_gen9.hpp,
// HwContext): i915's Skylake workarounds and register whitelist
// (gt/intel_workarounds.c), the fuses (gt/intel_sseu.c) and the null-state
// batch (gt/intel_renderstate.c), all MIT. The tables and the arithmetic are
// in intel_gen9_render_logic.hpp; this applies them, reads them back and
// logs what holds.

#include <graphics/intel_gen9.hpp>

#include <cpu/cereal/cereal.hpp>
#include <memory/vmm.hpp>
#include <common/string.hpp>

namespace intel_gen9 {

namespace {

void log(const char* text) {
    Cereal::get().write(text);
}

void logHex32(uint32_t value) {
    static const char kDigits[] = "0123456789abcdef";
    char text[11] = {'0', 'x'};
    for (int i = 0; i < 8; ++i) {
        text[2 + i] = kDigits[(value >> ((7 - i) * 4)) & 0xF];
    }
    text[10] = '\0';
    log(text);
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

void logMismatch(const char* what, const Workaround& w, uint32_t readBack) {
    log("[i915]   ");
    log(what);
    log(" ");
    logHex32(w.reg);
    log(" reads ");
    logHex32(readBack);
    log(w.masked ? ", masked write " : ", wants set ");
    logHex32(w.set);
    if (!w.masked) {
        log(" clear ");
        logHex32(w.clear);
    }
    log("\n");
}

void flushLines(const void* start, uint64_t bytes) {
    const auto* p = static_cast<const volatile uint8_t*>(start);
    for (uint64_t offset = 0; offset < bytes; offset += 64) {
        asm volatile("clflush (%0)" :: "r"(p + offset) : "memory");
    }
    asm volatile("mfence" ::: "memory");
}

} // namespace

const Gen9Sseu* IntelGen9Driver::topology() {
    if (sseuRead) {
        return &sseu;
    }
    if (!initialized || !forcewakeGet()) {
        return nullptr;
    }
    uint32_t euDisable[3];
    for (uint32_t slice = 0; slice < 3; ++slice) {
        euDisable[slice] = readReg(GEN9_EU_DISABLE0 + slice * 4);
    }
    sseu = decodeGen9Sseu(readReg(GEN8_FUSE2), euDisable);
    forcewakePut();
    sseuRead = true;
    return &sseu;
}

// i915's gen9_read_clock_frequency: from the crystal (24 MHz on big-core
// parts) shifted by CTC_MODE's shift field, or the reference clock that
// TIMESTAMP_OVERRIDE describes.
uint32_t IntelGen9Driver::timestampFrequency() {
    if (timestampHz != 0) {
        return timestampHz;
    }
    constexpr uint32_t CTC_MODE = 0xA26C;
    constexpr uint32_t GEN9_TIMESTAMP_OVERRIDE = 0x44074;
    if (!initialized || !forcewakeGet()) {
        return 0;
    }
    const uint32_t ctc = readReg(CTC_MODE);
    uint32_t hz = 0;
    if ((ctc & 1u) != 0) {   // CTC_SOURCE_DIVIDE_LOGIC
        const uint32_t override = readReg(GEN9_TIMESTAMP_OVERRIDE);
        const uint32_t base = ((override & 0x3FFu) + 1) * 1000000u;
        const uint32_t frac = 1000000u / (((override >> 12) & 0xFu) + 1);
        hz = base + frac;
    } else {
        hz = 24000000u >> (3 - ((ctc >> 1) & 0x3u));
    }
    forcewakePut();
    log("[i915] command streamer timestamps: CTC_MODE ");
    logHex32(ctc);
    log(", ");
    logDec(hz);
    log(" Hz\n");
    if (hz < 1000000u || hz > 100000000u) {
        hz = 12000000u;   // what Skylake runs at; the registers made no sense
    }
    timestampHz = hz;
    return hz;
}

bool IntelGen9Driver::prepareRendering() {
    if (renderingPrepared) {
        return true;
    }
    if (!patProgrammed) {
        programPrivatePat();
    }

    // The fuses: which slices, subslices and EUs this part has.
    uint32_t euDisable[3];
    for (uint32_t slice = 0; slice < 3; ++slice) {
        euDisable[slice] = readReg(GEN9_EU_DISABLE0 + slice * 4);
    }
    const uint32_t fuse2 = readReg(GEN8_FUSE2);
    sseu = decodeGen9Sseu(fuse2, euDisable);
    sseuRead = true;
    rpcs = gen9Rpcs(sseu);
    const uint8_t revision = static_cast<uint8_t>(readConfig32(0x08) & 0xFF);
    log("[i915] fuses: FUSE2 ");
    logHex32(fuse2);
    log(", slices ");
    logHex32(sseu.sliceMask);
    log(", subslices ");
    logHex32(sseu.subsliceMask);
    log(", ");
    logDec(sseu.euTotal);
    log(" EUs (slice 0:");
    for (uint32_t ss = 0; ss < 4; ++ss) {
        log(" ");
        logDec(sseu.eus[0][ss]);
    }
    log("), revision ");
    logDec(revision);
    log("; render contexts get R_PWR_CLK_STATE ");
    logHex32(rpcs);
    log("\n");

    // GT workarounds: read, modify, write, then check they took.
    Workaround gt[kSklGtWorkaroundCount + 2];
    const uint32_t count = gen9GtWorkarounds(gt, sseu, revision);
    for (uint32_t i = 0; i < count; ++i) {
        writeReg(gt[i].reg, workaroundValue(gt[i], readReg(gt[i].reg)));
    }
    uint32_t holding = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t readBack = readReg(gt[i].reg);
        if (workaroundHolds(gt[i], readBack)) {
            ++holding;
        } else {
            logMismatch("GT workaround", gt[i], readBack);
        }
    }
    log("[i915] GT workarounds: ");
    logDec(count);
    log(" set, ");
    logDec(holding);
    log(" hold\n");

    contextWorkaroundCount = gen9ContextWorkarounds(contextWorkarounds, sseu);

    // The null-state batch, relocated to its GGTT address.
    nullState = createBuffer(GTT_PAGE_SIZE, GTT_PAGE_SIZE, false);
    workaroundCheck = createBuffer(GTT_PAGE_SIZE, GTT_PAGE_SIZE, false);
    if (!nullState || !workaroundCheck) {
        log("[i915] rendering: could not create the null-state buffers\n");
        return false;
    }
    auto* words = reinterpret_cast<uint32_t*>(VMM::PhysToVirt(nullState->pages[0]));
    if (!relocateNullState(words, nullState->gttOffset)) {
        log("[i915] rendering: the null-state batch does not relocate\n");
        return false;
    }
    flushLines(words, GTT_PAGE_SIZE);

    // The indirect context batch at the start of its page, the L3SQCREG4
    // save slot it uses at the end.
    indirectCtx = createBuffer(GTT_PAGE_SIZE, GTT_PAGE_SIZE, false);
    if (!indirectCtx) {
        log("[i915] rendering: could not create the indirect context batch\n");
        return false;
    }
    auto* wa = reinterpret_cast<uint32_t*>(VMM::PhysToVirt(indirectCtx->pages[0]));
    memset(wa, 0, GTT_PAGE_SIZE);
    const uint32_t* waEnd = emitGen9IndirectCtx(wa, static_cast<uint32_t>(indirectCtx->gttOffset + GTT_PAGE_SIZE - 64));
    indirectCtxBytes = static_cast<uint32_t>(waEnd - wa) * 4;
    flushLines(wa, GTT_PAGE_SIZE);
    log("[i915] indirect context batch: ");
    logDec(indirectCtxBytes);
    log(" bytes at GGTT ");
    logHex32(static_cast<uint32_t>(indirectCtx->gttOffset));
    log("\n");

    log("[i915] null-state batch: ");
    logDec(kGen9NullStateDwords);
    log(" dwords at GGTT ");
    logHex32(static_cast<uint32_t>(nullState->gttOffset));
    log(", ");
    logDec(contextWorkaroundCount);
    log(" context workarounds\n");
    renderingPrepared = true;
    return true;
}

void IntelGen9Driver::applyEngineWorkarounds(EngineState& e) {
    static bool reported[kEngineCount] = {};
    const int index = &e == &engines[0] ? 0 : 1;
    if (e.base != RENDER_RING_BASE) {
        e.workaroundsApplied = true;   // Skylake has none for the blitter
        return;
    }
    for (uint32_t i = 0; i < kSklRenderWorkaroundCount; ++i) {
        const Workaround& w = kSklRenderWorkarounds[i];
        writeReg(w.reg, workaroundValue(w, readReg(w.reg)));
    }
    for (uint32_t slot = 0; slot < RING_MAX_NONPRIV_SLOTS; ++slot) {
        writeReg(ringForceToNonpriv(e.base, slot),
                 slot < kSklRenderWhitelistCount ? kSklRenderWhitelist[slot] : ringNopid(e.base));
    }

    uint32_t holding = 0;
    for (uint32_t i = 0; i < kSklRenderWorkaroundCount; ++i) {
        const Workaround& w = kSklRenderWorkarounds[i];
        const uint32_t readBack = readReg(w.reg);
        if (workaroundHolds(w, readBack)) {
            ++holding;
        } else {
            logMismatch("render workaround", w, readBack);
        }
    }
    uint32_t listed = 0;
    for (uint32_t slot = 0; slot < kSklRenderWhitelistCount; ++slot) {
        // RING_FORCE_TO_NONPRIV keeps the address in bits 25:2.
        if ((readReg(ringForceToNonpriv(e.base, slot)) & 0x03FFFFFCu) == kSklRenderWhitelist[slot]) {
            ++listed;
        }
    }
    const bool ok = holding == kSklRenderWorkaroundCount && listed == kSklRenderWhitelistCount;
    if (!reported[index] || !ok) {
        reported[index] = true;
        log("[i915] render workarounds: ");
        logDec(kSklRenderWorkaroundCount);
        log(" set, ");
        logDec(holding);
        log(" hold; whitelist: ");
        logDec(listed);
        log(" of ");
        logDec(kSklRenderWhitelistCount);
        log(" registers open to user batches\n");
    }
    e.workaroundsApplied = true;
}

void IntelGen9Driver::verifyContextWorkarounds() {
    auto* words = reinterpret_cast<volatile uint32_t*>(VMM::PhysToVirt(workaroundCheck->pages[0]));
    flushLines(const_cast<uint32_t*>(words), contextWorkaroundCount * 4);
    uint32_t holding = 0;
    for (uint32_t i = 0; i < contextWorkaroundCount; ++i) {
        if (workaroundHolds(contextWorkarounds[i], words[i])) {
            ++holding;
        } else {
            logMismatch("context workaround", contextWorkarounds[i], words[i]);
        }
    }
    log("[i915] context workarounds: ");
    logDec(contextWorkaroundCount);
    log(" set in the first render context, ");
    logDec(holding);
    log(" hold\n");
}

} // namespace intel_gen9
