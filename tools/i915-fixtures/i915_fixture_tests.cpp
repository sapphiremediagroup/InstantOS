// Host-side tests of the Intel Gen9 driver's pure logic
// (include/graphics/intel_gen9_logic.hpp): device ids, BAR and plane decoding,
// the GGC/BDSM decode, GGTT entries and the GTT range allocator. The register
// values come from the HD 530 PC's /kernel.log.

#include <graphics/intel_gen9_logic.hpp>
#include <graphics/intel_gen9_render_logic.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace intel_gen9;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("FAIL: %s\n", what);
    }
}

void testDevices() {
    const Gen9DeviceInfo* hd530 = lookupDevice(0x8086, 0x1912);
    check(hd530 && hd530->isHd530 && hd530->gt == Gen9Gt::Gt2, "0x1912 is HD 530 GT2");
    check(isHd530(0x191B), "0x191B is HD 530");
    check(!isHd530(0x1916), "0x1916 is not HD 530");
    check(lookupDevice(0x8086, 0x1932) && lookupDevice(0x8086, 0x1932)->gt == Gen9Gt::Gt4, "0x1932 is GT4");
    check(lookupDevice(0x10DE, 0x1912) == nullptr, "another vendor is not Gen9");
    check(lookupDevice(0x8086, 0x3E92) == nullptr, "a Coffee Lake id is not in the Skylake table");
}

void testBars() {
    // The PC: GTTMMADR=0xd0000000, GMADR=0xc0000000 (64-bit BARs; GMADR prefetchable).
    check(decodeMemoryBar(0xd0000004, 0) == 0xd0000000, "64-bit BAR0");
    check(decodeMemoryBar(0xc000000c, 0) == 0xc0000000, "64-bit prefetchable BAR2");
    check(decodeMemoryBar(0x0000000c, 0x4) == 0x400000000ULL, "64-bit BAR above 4 GiB");
    check(decodeMemoryBar(0xd0000000, 0x4) == 0xd0000000, "32-bit BAR ignores the next dword");
    check(decodeMemoryBar(0x0000f001, 0) == 0, "I/O BAR");
    check(decodeMemoryBar(0, 0) == 0 && decodeMemoryBar(0xFFFFFFFF, 0) == 0, "unset BAR");
}

void testPlane() {
    // The PC: PLANE_CTL_A=0x84000000 (on, XRGB8888, linear), 1024x768, 4096-byte rows, surface 0.
    const uint32_t srcsz = (1023u << 16) | 767u;
    DisplayMode m = interpretPlane(0x84000000, 4096 / 64, srcsz, 0, 800, 600, 3200);
    check(m.valid && m.width == 1024 && m.height == 768 && m.stride == 4096 && m.surfaceGtt == 0,
          "the PC's plane: 1024x768, stride 4096, surface 0");
    m = interpretPlane(0x84000000 | (1u << 10), 4096 / 64, srcsz, 0x40000, 800, 600, 3200);
    check(m.valid && m.width == 800 && m.stride == 3200, "an X-tiled plane falls back to the GOP geometry");
    m = interpretPlane(0x04000000, 4096 / 64, srcsz, 0, 800, 600, 0);
    check(m.valid && m.width == 800 && m.stride == 3200, "a disabled plane falls back (stride from the width)");
    m = interpretPlane(0x84000000, 1, srcsz, 0x123456, 0, 0, 0);
    check(m.valid && m.stride == 4096 && m.surfaceGtt == 0x123000, "a short stride is widened, the surface page-aligned");
    m = interpretPlane(0x04000000, 0, 0, 0, 0, 0, 0);
    check(!m.valid, "nothing usable");
}

void testGgc() {
    // The PC: GGC=0x10c1 -> an 8 MiB GTT (4 GiB of GGTT space), 512 MiB stolen.
    GgcInfo g = decodeGgc(0x10c1);
    check(g.gsmBytes == 8ULL << 20, "GGC 0x10c1: 8 MiB GTT");
    check(g.ggttBytes == 4ULL << 30, "GGC 0x10c1: 4 GiB GGTT");
    check(g.stolenBytes == 512ULL << 20, "GGC 0x10c1: 512 MiB stolen");
    check(decodeGgc(0x0140).gsmBytes == 2ULL << 20 && decodeGgc(0x0140).stolenBytes == 32ULL << 20,
          "GGMS 1, GMS 1: 2 MiB GTT, 32 MiB stolen");
    check(decodeGgc(0xF080).gsmBytes == 4ULL << 20 && decodeGgc(0xF080).stolenBytes == 4ULL << 20,
          "GMS 0xF0: 4 MiB stolen");
    check(decodeGgc(0xF380).stolenBytes == 16ULL << 20, "GMS 0xF3: 16 MiB stolen");
    check(decodeGgc(0x0000).gsmBytes == 0 && decodeGgc(0x0000).ggttBytes == 0, "no GTT");
    check(decodeBdsm(0x9e800001) == 0x9e800000, "BDSM drops the lock bit");
    check(GSM_OFFSET_IN_BAR0 == 8ULL << 20, "the GTT is the upper half of a 16 MiB BAR0");
}

void testPte() {
    check(encodePte(0x12345000) == 0x12345001, "entry = address | present");
    check(encodePte(0x12345678) == 0x12345001, "the page offset is dropped");
    check(encodePte(0x8000000012345000ULL) == 0x12345001, "bits above 39 are dropped");
    check(pteValid(0x9e800001) && !pteValid(0x9e800000), "present bit");
    check(ptePhys(0x9e800001) == 0x9e800000, "entry -> address");
}

void testCommands() {
    check(maskedEnable(STOP_RING) == 0x01000100 && maskedDisable(STOP_RING) == 0x01000000, "masked bits");
    check(MI_STORE_DWORD_IMM_GEN4 == 0x10000002, "MI_STORE_DWORD_IMM is opcode 0x20, length 2");
    check((MI_FLUSH_DW + 1) == 0x13000002, "MI_FLUSH_DW with a 64-bit address is 4 dwords");
    check(MI_SEMAPHORE_WAIT == 0x0e000002, "MI_SEMAPHORE_WAIT is opcode 0x1c");
    check(miLoadRegisterImm(14) == 0x1100001b, "LRI of 14 registers");
    check(MI_BATCH_BUFFER_END == 0x05000000, "MI_BATCH_BUFFER_END");
    check(contextDescriptor(0x00345000, 7) == 0x0000000700345109ULL,
          "descriptor: lrca | privileged | legacy 32-bit | valid, id above");
    check(contextDescriptor(0x00345678, 0) == 0x0000000000345109ULL, "the lrca is page aligned");
}

// The register state follows gen9_xcs_offsets, at the dword positions that
// i915's CTX_* constants (intel_lrc_reg.h) assume.
void testContextImage() {
    std::vector<uint32_t> regs(LRC_STATE_DWORDS, 0xFFFFFFFFu);
    ContextSetup s = {};
    s.layout = kGen9XcsOffsets;
    s.engineBase = BLT_RING_BASE;
    s.ringGgtt = 0x00500000;
    s.ringBytes = 4096;
    s.head = 0;
    s.tail = 32;
    s.pdp[0] = 0x1000;
    s.pdp[1] = 0x2000;
    s.pdp[2] = 0x3000;
    s.pdp[3] = 0x400004000ULL;
    buildContextState(regs.data(), s);
    const uint32_t b = BLT_RING_BASE;
    check(regs[0] == MI_NOOP, "dword 0 is a NOP");
    check(regs[1] == (miLoadRegisterImm(14) | MI_LRI_FORCE_POSTED), "LRI(14, posted) at 0x01");
    check(regs[0x02] == b + 0x244 && regs[0x04] == b + 0x34 && regs[0x06] == b + 0x30 &&
          regs[0x08] == b + 0x38 && regs[0x0a] == b + 0x3c, "context control, head, tail, start, ctl");
    check(regs[0x1c] == b + 0x1c8, "the 14th register (BB_PER_CTX_PTR) at 0x1c");
    check(regs[0x1e] == 0 && regs[0x20] == 0, "NOP(3) after it");
    check(regs[0x21] == (miLoadRegisterImm(9) | MI_LRI_FORCE_POSTED), "LRI(9, posted) at 0x21 (CTX_LRI_HEADER_1)");
    check(regs[0x22] == b + 0x3a8 && regs[0x24] == b + 0x28c && regs[0x32] == b + 0x270,
          "timestamp, PDP3_UDW, PDP0_LDW addresses");
    check(regs[0x41] == (miLoadRegisterImm(1) | MI_LRI_FORCE_POSTED) && regs[0x42] == b + 0x200,
          "LRI(1) at 0x41 (CTX_LRI_HEADER_2), R_PWR_CLK_STATE");
    check(regs[0x51] == (miLoadRegisterImm(44) | MI_LRI_FORCE_POSTED), "LRI(44) at 0x51");
    check(regs[0x52] == b + 0x28 && regs[0x54] == b + 0x9c, "the MI_MODE register at 0x54 (GEN9_CTX_RING_MI_MODE)");
    check(regs[0xa8] == b + 0x68 && regs[0xaa] == MI_BATCH_BUFFER_END, "the last register, then MI_BATCH_BUFFER_END");
    check(regs[0xab] == 0 && regs[LRC_STATE_DWORDS - 1] == 0, "the rest is zero");

    check(regs[CTX_RING_START] == 0x00500000 && regs[CTX_RING_HEAD] == 0 && regs[CTX_RING_TAIL] == 32,
          "ring start, head, tail values");
    check(regs[CTX_RING_CTL] == RING_VALID, "a one-page ring: size field 0, valid");
    check(regs[CTX_CONTEXT_CONTROL] == 0x000f0009,
          "context control: inhibit-syn-switch and restore-inhibit on, save-inhibit and RS off");
    check(regs[CTX_TIMESTAMP] == 0, "timestamp 0");
    check(regs[0x25] == 0x4 && regs[0x27] == 0x4000 && regs[0x2b] == 0x3000 &&
          regs[0x2f] == 0x2000 && regs[0x33] == 0x1000 && regs[0x31] == 0,
          "PDP3..PDP0 upper and lower halves");
    check(regs[GEN9_CTX_RING_MI_MODE + 1] == maskedDisable(STOP_RING), "MI_MODE value: STOP_RING cleared");

    // The render engine's layout: the same, but the third block is a plain
    // LRI of R_PWR_CLK_STATE (0xc8), left 0.
    std::vector<uint32_t> rcs(LRC_STATE_DWORDS, 0xFFFFFFFFu);
    ContextSetup r = s;
    r.layout = kGen9RcsOffsets;
    r.engineBase = RENDER_RING_BASE;
    buildContextState(rcs.data(), r);
    const uint32_t rb = RENDER_RING_BASE;
    check(rcs[0x01] == (miLoadRegisterImm(14) | MI_LRI_FORCE_POSTED) && rcs[0x02] == rb + 0x244 &&
          rcs[0x08] == rb + 0x38, "render: the ring block at the same places, render base");
    check(rcs[0x41] == miLoadRegisterImm(1) && rcs[0x42] == rb + 0xc8 && rcs[0x43] == 0,
          "render: LRI(1) of R_PWR_CLK_STATE, not posted, value 0");
    check(rcs[0x51] == (miLoadRegisterImm(44) | MI_LRI_FORCE_POSTED) && rcs[0x54] == rb + 0x9c &&
          rcs[0xa8] == rb + 0x68 && rcs[0xaa] == MI_BATCH_BUFFER_END, "render: the rest as on the blitter");
    check(rcs[CTX_RING_START] == 0x00500000 && rcs[CTX_RING_TAIL] == 32 &&
          rcs[CTX_CONTEXT_CONTROL] == 0x000f0009, "render: ring and context control values");
    check(MI_USER_INTERRUPT == 0x01000000, "MI_USER_INTERRUPT is opcode 2");
}

// Step 6b: what real work on the engines needs (intel_gen9_render_logic.hpp).
void testRender() {
    // The null-state batch: fits a page, ends with MI_BATCH_BUFFER_END, and
    // its four relocations become qwords pointing into its own copy.
    check(kGen9NullStateDwords == 960 && kGen9NullStateDwords * 4 <= 4096, "null state: 960 dwords, one page");
    check(kGen9NullStateBatch[885] == MI_BATCH_BUFFER_END, "null state: the commands end at dword 885");
    std::vector<uint32_t> batch(kGen9NullStateDwords);
    const uint64_t ggtt = 0x1234567000ULL;
    check(relocateNullState(batch.data(), ggtt), "null state relocates");
    bool relocated = true;
    for (uint32_t r = 0; r < kGen9NullStateRelocCount; ++r) {
        const uint32_t i = kGen9NullStateRelocs[r] / 4;
        const uint64_t want = kGen9NullStateBatch[i] + ggtt;
        relocated = relocated && batch[i] == static_cast<uint32_t>(want) && batch[i + 1] == static_cast<uint32_t>(want >> 32);
    }
    check(relocated, "each relocation is the batch offset plus its address, upper half next");
    uint32_t changed = 0;
    for (uint32_t i = 0; i < kGen9NullStateDwords; ++i) {
        changed += batch[i] != kGen9NullStateBatch[i];
    }
    check(changed == 8, "only the four relocated qwords differ from the table");

    // Workarounds: values and read-back checks.
    const Workaround& csChicken = kSklContextWorkarounds[0];
    check(csChicken.reg == 0x2580 && csChicken.set == 0x00070002 && csChicken.masked,
          "CS_CHICKEN1: object-level preemption off, GPGPU command level (merged, 0x00070002)");
    check(workaroundHolds(csChicken, 0x2) && !workaroundHolds(csChicken, 0x3) && !workaroundHolds(csChicken, 0x4),
          "a masked setting holds only with exactly its bits");
    const Workaround scratch1 = kSklRenderWorkarounds[5];
    check(scratch1.reg == 0xb11c && workaroundValue(scratch1, 0x101) == 0x5 && workaroundHolds(scratch1, 0x5) &&
          !workaroundHolds(scratch1, 0x105), "BDW_SCRATCH1: bit 2 set, bit 8 cleared");
    check(kSklContextWorkaroundCount == 9 && kSklRenderWorkaroundCount == 6 && kSklRenderWhitelistCount == 5,
          "9 context, 6 render workarounds, 5 whitelisted registers");
    check(ringForceToNonpriv(RENDER_RING_BASE, 0) == 0x24d0 && ringForceToNonpriv(RENDER_RING_BASE, 11) == 0x24fc &&
          ringNopid(RENDER_RING_BASE) == 0x2094, "RING_FORCE_TO_NONPRIV slots and RING_NOPID");

    // Fuses: a GT2 with one slice, three subslices of 8 EUs.
    const uint32_t full[3] = {0, 0, 0};
    const Gen9Sseu gt2 = decodeGen9Sseu((1u << 25) | (0x8u << 20), full);
    check(gt2.sliceMask == 1 && gt2.subsliceMask == 0x7 && gt2.euTotal == 24 && gt2.eus[0][3] == 0,
          "GT2 fuses: 1 slice, subslices 0-2, 24 EUs");
    check(gen9Rpcs(gt2) == 0x80000088, "GT2 RPCS: EU power gating asks for 8 of 8");
    check(gen9IzHashing(gt2) == 0, "no 7-EU subslice: no IZ hashing");
    const Workaround mcr = gen9McrSteering(gt2);
    check(mcr.reg == 0xfdc && mcr.clear == 0x0f000000 && mcr.set == 0, "MCR steering to slice 0, subslice 0");
    // One EU of subslice 1 fused off: the hash leans away from it.
    const uint32_t oneOff[3] = {0x00000100, 0, 0};
    const Gen9Sseu gt2b = decodeGen9Sseu((1u << 25) | (0x8u << 20), oneOff);
    check(gt2b.euTotal == 23 && gt2b.subslice7eu[0] == 0x2, "23 EUs, subslice 1 has 7");
    check(gen9IzHashing(gt2b) == 0x003f0002, "IZ hashing for slice 0: 3 - 1 = 2");
    Workaround ctx[kSklContextWorkaroundCount + 1];
    check(gen9ContextWorkarounds(ctx, gt2) == 9 && gen9ContextWorkarounds(ctx, gt2b) == 10 &&
          ctx[9].reg == GEN7_GT_MODE && ctx[9].set == 0x003f0002, "the context list gains GT_MODE when needed");
    // The topology query for the 23-EU part (the HD 530 PC's fuses).
    uint8_t topo[kGen9TopologyBytes + 4] = {};
    check(encodeGen9Topology(gt2b, nullptr, 0) == 32 && encodeGen9Topology(gt2b, topo, sizeof(topo)) == 32,
          "topology query: 32 bytes (16 header, 1 slice, 3 subslice, 12 EU masks)");
    check(topo[2] == 3 && topo[4] == 4 && topo[6] == 8 && topo[8] == 1 && topo[10] == 1 && topo[12] == 4 &&
              topo[14] == 1,
          "header: 3 slices, 4 subslices, 8 EUs at most; offsets 1 and 4, strides 1");
    check(topo[16] == 0x1 && topo[17] == 0x7 && topo[18] == 0 && topo[20] == 0xFF && topo[21] == 0xFE &&
              topo[22] == 0xFF && topo[23] == 0 && topo[24] == 0,
          "slice 0, subslices 0-2, EU masks FF FE FF (subslice 1's EU 0 fused off)");
    const uint32_t ss0Off[3] = {0, 0, 0};
    const Gen9Sseu noSs0 = decodeGen9Sseu((1u << 25) | (0x1u << 20), ss0Off);
    check(gen9McrSteering(noSs0).set == (1u << 24), "subslice 0 fused off: steer to subslice 1");
    Workaround gt[kSklGtWorkaroundCount + 2];
    check(gen9GtWorkarounds(gt, gt2, 6) == 7 && gt[6].reg == GEN9_GAMT_ECO_REG_RW_IA && gen9GtWorkarounds(gt, gt2, 9) == 6 &&
          gt[0].reg == GEN8_MCR_SELECTOR, "GT list: MCR first, WaInPlaceDecompressionHang only before H0");

    // Ring commands.
    uint32_t cs[64] = {};
    uint32_t* end = emitFlushRcs(cs, false, true);
    check(end - cs == 12 && cs[0] == 0x7a000004 && cs[1] == 0, "invalidate: a null PIPE_CONTROL first (Gen9)");
    check((cs[7] & PIPE_CONTROL_TLB_INVALIDATE) && (cs[7] & PIPE_CONTROL_STORE_DATA_INDEX) &&
          (cs[7] & PIPE_CONTROL_CS_STALL) && cs[8] == 0xd0, "invalidate: TLBs and caches, post-sync to the scratch dword");
    end = emitFlushRcs(cs, true, false);
    check(end - cs == 6 && (cs[1] & PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH) && !(cs[1] & PIPE_CONTROL_TLB_INVALIDATE),
          "flush only: one PIPE_CONTROL");
    end = emitFlushXcs(cs, true);
    check(end - cs == 4 && cs[0] == 0x13244002 && cs[1] == 0xd0, "MI_FLUSH_DW with the TLB invalidation");
    end = emitBatchStart(cs, 0x0000800000100000ULL, true);
    check(end - cs == 6 && cs[0] == 0x04000001 && cs[1] == 0x18800101 && cs[2] == 0x00100000 && cs[3] == 0x8000 &&
          cs[4] == 0x04000000, "batch start from the PPGTT, arbitration on around it");
    end = emitBatchStart(cs, 0x00abc000, false);
    check(cs[1] == 0x18800001, "a privileged batch from the GGTT");
    end = emitBreadcrumb(cs, true, 77, 0x40);
    check(end - cs == 14 && (cs[1] & PIPE_CONTROL_GLOBAL_GTT_IVB) && (cs[1] & PIPE_CONTROL_QW_WRITE) &&
          (cs[1] & PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH) && (cs[1] & PIPE_CONTROL_DC_FLUSH_ENABLE) &&
          !(cs[1] & PIPE_CONTROL_TLB_INVALIDATE) && cs[2] == 0x40 && cs[4] == 77 &&
          (cs[7] & PIPE_CONTROL_CS_STALL) && (cs[7] & PIPE_CONTROL_FLUSH_ENABLE) && cs[12] == 0x04000001,
          "render breadcrumb: the seqno's GGTT qword write flushes the caches, then a CS-stall flush");
    end = emitBreadcrumb(cs, false, 78, 0x40);
    check(end - cs == 6 && cs[0] == 0x13004002 && cs[1] == 0x44 && cs[3] == 78, "blitter breadcrumb: MI_FLUSH_DW store");
    end = emitContextWorkarounds(cs, kSklContextWorkarounds, kSklContextWorkaroundCount);
    check(end - cs == 20 && cs[0] == miLoadRegisterImm(9) && cs[1] == 0x2580 && cs[2] == 0x00070002 && cs[19] == MI_NOOP,
          "the context workarounds: one LRI of 9");
    end = emitStoreRegisterGgtt(cs, 0x2580, 0x1000);
    check(end - cs == 4 && cs[0] == 0x12400002 && cs[1] == 0x2580 && cs[2] == 0x1000, "SRM to the GGTT");
    {
        uint32_t wa[64] = {};
        const uint32_t* waEnd = emitGen9IndirectCtx(wa, 0x2000);
        const long n = waEnd - wa;
        check(n == 48 && wa[0] == (MI_ARB_ON_OFF | MI_ARB_DISABLE) &&
              wa[1] == 0x12400002 && wa[2] == GEN8_L3SQCREG4 && wa[3] == 0x2000 &&
              wa[5] == miLoadRegisterImm(1) && wa[6] == GEN8_L3SQCREG4 && wa[7] == 0x40600000 &&
              wa[8] == gfxOpPipeControl(6) && wa[9] == (PIPE_CONTROL_CS_STALL | PIPE_CONTROL_DC_FLUSH_ENABLE) &&
              wa[14] == 0x14c00002 && wa[15] == GEN8_L3SQCREG4 && wa[16] == 0x2000 &&
              (wa[19] & PIPE_CONTROL_FLUSH_L3) && wa[20] == LRC_PPHWSP_SCRATCH_ADDR &&
              wa[24] == miLoadRegisterImm(3) && wa[25] == 0x7014 && wa[26] == 0x10000000 &&
              wa[27] == 0x2088 && wa[28] == 0x00020002 && wa[29] == 0x2090 && wa[30] == 0x10001000 &&
              wa[31] == MI_NOOP && wa[32] == (MI_ARB_ON_OFF | MI_ARB_ENABLE),
              "indirect context batch: coherent L3 flush around a saved L3SQCREG4, SLM clear, 3 LRIs, cache-line padded");
        std::vector<uint32_t> ictx(LRC_STATE_DWORDS, 0);
        setLrcOffsets(ictx.data(), kGen9RcsOffsets, RENDER_RING_BASE, true);
        setIndirectCtx(ictx.data(), 0x00abc000, 128);
        check(ictx[CTX_BB_PER_CTX_PTR - 1] == RENDER_RING_BASE + 0x1c0 && ictx[CTX_INDIRECT_CTX - 1] == RENDER_RING_BASE + 0x1c4 &&
              ictx[CTX_INDIRECT_CTX_OFFSET - 1] == RENDER_RING_BASE + 0x1c8 && ictx[CTX_INDIRECT_CTX] == 0x00abc002 &&
              ictx[CTX_INDIRECT_CTX_OFFSET] == 0x26u << 6,
              "indirect context registers in the render layout: batch | cache lines, offset 0x26");
    }

    // A context running again: only the ring window and control change.
    std::vector<uint32_t> regs(LRC_STATE_DWORDS, 0x5A5A5A5Au);
    prepareContextResubmit(regs.data(), 0x100, 0x180);
    check(regs[CTX_RING_HEAD] == 0x100 && regs[CTX_RING_TAIL] == 0x180 && regs[CTX_CONTEXT_CONTROL] == 0x000f0008 &&
          regs[CTX_RING_START] == 0x5A5A5A5Au, "resubmit: head, tail, restore no longer inhibited, the rest untouched");
    check(ringPlacement(100, 64, 16384) == 100 && ringPlacement(16340, 64, 16384) == 0 && ringPlacement(16320, 64, 16384) == 16320,
          "ring placement wraps only when the commands would pass the end");

    // RPCS in a render context's image.
    std::vector<uint32_t> rcs(LRC_STATE_DWORDS, 0);
    ContextSetup r = {};
    r.layout = kGen9RcsOffsets;
    r.engineBase = RENDER_RING_BASE;
    r.ringGgtt = 0x00500000;
    r.ringBytes = 16384;
    r.fourLevel = true;
    r.pml4 = 0x7000;
    r.rpcs = 0x80000088;
    buildContextState(rcs.data(), r);
    check(rcs[0x42] == RENDER_RING_BASE + 0xc8 && rcs[CTX_R_PWR_CLK_STATE] == 0x80000088, "render image: R_PWR_CLK_STATE value");
    check(rcs[CTX_RING_CTL] == (3u * 4096 | RING_VALID), "a four-page ring: size field 3 pages");
    // The dword after the register list: BB_END only when restore is inhibited.
    check(rcs[0xaa] == MI_BATCH_BUFFER_END, "an inhibited image is closed after its register list");
    std::vector<uint32_t> fromDefault(LRC_STATE_DWORDS, 0x7a5a5a5au);
    buildContextState(fromDefault.data(), r, true);
    check(fromDefault[0xaa] == 0x7a5a5a5au && fromDefault[0x51] == (miLoadRegisterImm(44) | MI_LRI_FORCE_POSTED),
          "an image from the default keeps what follows the register list (the render state the engine restores)");
}

// Step 6a: the private PAT, PPGTT entries and a 4-level context.
void testPpgtt() {
    // i915's bdw_setup_private_ppat on Gen9, byte by byte: WB|LLC, WC|LLCeLLC,
    // WB (eLLC override), UC, then WB|LLCeLLC with ages 0..3.
    check(kGen9PrivatePat == 0x3B2B1B0B00030907ULL, "the private PAT is i915's (0x3b2b1b0b_00030907)");
    // An entry's PWT (3), PCD (4) and PAT (7) bits pick the PPAT index.
    auto ppatIndex = [](uint64_t entry) {
        return static_cast<int>(((entry >> 7) & 1) << 2 | ((entry >> 4) & 1) << 1 | ((entry >> 3) & 1));
    };
    check(ppatIndex(ppgttPte(0x1000, true)) == 4, "a cached page is PPAT index 4 (WB, LLC and eLLC)");
    check(ppatIndex(ppgttPte(0x1000, false)) == 3, "an uncached page is PPAT index 3 (UC)");
    check(ppatIndex(ppgttPde(0x1000)) == 0, "a table is PPAT index 0 (WB, LLC)");
    check(ppgttPte(0x123456789000ULL, true) == (0x123456789000ULL | 0x83), "PTE: address | present | rw | PAT");
    check(ppgttPde(0x0000000ABCDE000ULL) == 0x0000000ABCDE003ULL, "PDE: address | present | rw");
    check(ppgttEntryAddress(ppgttPte(0x7FFFFFFFF000ULL, false)) == 0x7FFFFFFFF000ULL, "the address comes back out");
    const uint64_t va = 0x00007FFFFFFF0000ULL;
    check(ppgttIndex(va, 3) == 255 && ppgttIndex(va, 2) == 511 && ppgttIndex(va, 1) == 511 &&
          ppgttIndex(va, 0) == 496, "0x7fffffff0000 walks PML4 255, PDP 511, PD 511, PT 496");
    check(ppgttIndex(0x0000123456780000ULL, 3) == 0x24 && ppgttIndex(0x0000123456780000ULL, 0) == 0x180,
          "0x123456780000 walks PML4 36 ... PT 384");

    check(contextDescriptor(0x00345000, 7, true) == 0x0000000700345119ULL,
          "a 4-level context's descriptor: addressing mode 3 (legacy 64-bit)");
    std::vector<uint32_t> regs(LRC_STATE_DWORDS, 0xFFFFFFFFu);
    ContextSetup s = {};
    s.layout = kGen9RcsOffsets;
    s.engineBase = RENDER_RING_BASE;
    s.ringGgtt = 0x00500000;
    s.ringBytes = 4096;
    s.pdp[1] = 0x2000;   // ignored with fourLevel
    s.fourLevel = true;
    s.pml4 = 0x0000000123456000ULL;
    buildContextState(regs.data(), s);
    check(regs[CTX_PDP0_LDW] == 0x23456000 && regs[CTX_PDP0_LDW - 2] == 0x1,
          "PDP0 holds the PML4 (i915's ASSIGN_CTX_PML4)");
    check(regs[CTX_PDP3_UDW] == 0 && regs[CTX_PDP3_UDW + 2] == 0 && regs[CTX_PDP3_UDW + 8] == 0 &&
          regs[CTX_PDP3_UDW + 10] == 0, "PDP1..3 are zero");
    check(MI_BATCH_BUFFER_START_GEN8 == 0x18800001u && (MI_BATCH_BUFFER_START_GEN8 | MI_BATCH_PPGTT) == 0x18800101u,
          "MI_BATCH_BUFFER_START (Gen8, PPGTT): 0x18800101");
}

void testAllocatorBasics() {
    std::vector<uint64_t> bits(GttPageAllocator::wordsFor(1000));
    GttPageAllocator a;
    a.init(bits.data(), 1000);
    check(a.freePages() == 1000 && a.totalPages() == 1000, "init");
    check(a.alloc(10) == 0, "first fit at 0");
    check(a.alloc(10, 16) == 16, "aligned to 16 pages");
    check(a.freePages() == 980, "two allocations taken");
    check(a.alloc(5, 1, 20) == 10, "the gap below the limit");
    check(a.alloc(10, 1, 30) == GttPageAllocator::kNone, "nothing fits below the limit");
    check(a.free(16, 10) && a.free(0, 10) && a.free(10, 5), "frees");
    check(a.freePages() == 1000 && !a.isUsed(0) && !a.isUsed(999), "all free again");
    check(!a.free(100, 1) && a.freePages() == 1000, "double free refused");
    check(!a.free(995, 10), "a free past the end refused");
    a.reserve(400, 100);
    check(a.freePages() == 900 && a.isUsed(400) && a.isUsed(499) && !a.isUsed(500), "a reservation");
    a.reserve(450, 200);
    check(a.freePages() == 750, "an overlapping reservation takes only what is free");
    a.reserve(990, 50);
    check(a.freePages() == 740, "a reservation past the end is clipped");
    check(a.alloc(450) == GttPageAllocator::kNone, "no 450-page hole");
    check(a.alloc(400) == 0, "the 400 pages below the reservation");
    check(a.alloc(1) == 650, "next free page after the reservation");
    check(a.alloc(0) == GttPageAllocator::kNone, "zero pages");
    check(a.alloc(1, 1024) == GttPageAllocator::kNone, "an alignment past the end");
    GttPageAllocator empty;
    empty.init(nullptr, 1000);
    check(empty.alloc(1) == GttPageAllocator::kNone && empty.freePages() == 0, "no storage: nothing to allocate");

    // Word boundaries: runs that straddle 64-page words, and a full word skipped.
    std::vector<uint64_t> bits2(GttPageAllocator::wordsFor(256));
    GttPageAllocator b;
    b.init(bits2.data(), 256);
    b.reserve(0, 60);
    check(b.alloc(10) == 60, "a run across the first word boundary");
    b.reserve(70, 70);
    check(b.alloc(100) == 140, "past a reserved stretch spanning a whole word");
    check(b.alloc(17) == GttPageAllocator::kNone && b.alloc(16) == 240, "the tail of the last word");

    // Fragmentation cannot make a free fail: every other page taken, then all given back.
    std::vector<uint64_t> bits3(GttPageAllocator::wordsFor(100000));
    GttPageAllocator c;
    c.init(bits3.data(), 100000);
    bool allOk = true;
    for (uint32_t i = 0; i < 100000; ++i) {
        allOk = allOk && c.alloc(1) == i;
    }
    for (uint32_t i = 0; i < 100000; i += 2) {
        allOk = allOk && c.free(i, 1);
    }
    for (uint32_t i = 1; i < 100000; i += 2) {
        allOk = allOk && c.free(i, 1);
    }
    check(allOk && c.freePages() == 100000, "100000 single pages, freed in a fragmenting order");
}

// Random allocations, frees and reservations against a simple model.
void testAllocatorRandom() {
    constexpr uint32_t kPages = 4096;
    std::vector<uint64_t> bits(GttPageAllocator::wordsFor(kPages));
    GttPageAllocator a;
    a.init(bits.data(), kPages);
    std::vector<int> owner(kPages, 0);   // 0 free, -1 reserved, >0 allocation id
    struct Live { uint32_t start; uint32_t pages; int id; };
    std::vector<Live> live;
    std::srand(12345);
    int nextId = 1;
    bool ok = true;
    for (int step = 0; step < 20000 && ok; ++step) {
        const int op = std::rand() % 10;
        if (op < 5) {
            const uint32_t pages = 1 + std::rand() % 64;
            const uint32_t align = 1u << (std::rand() % 5);
            const uint32_t limit = (std::rand() % 4 == 0) ? 1024 : 0;
            const uint32_t start = a.alloc(pages, align, limit);
            if (start != GttPageAllocator::kNone) {
                if (start % align != 0 || (limit && start + pages > limit) || start + pages > kPages) {
                    std::printf("step %d: alloc(%u, %u, %u) gave %u\n", step, pages, align, limit, start);
                    ok = false;
                    break;
                }
                for (uint32_t i = start; i < start + pages; ++i) {
                    if (owner[i] != 0) {
                        std::printf("step %d: alloc gave page %u, which is %d\n", step, i, owner[i]);
                        ok = false;
                    }
                    owner[i] = nextId;
                }
                live.push_back({start, pages, nextId++});
            }
        } else if (op < 9 && !live.empty()) {
            const size_t pick = std::rand() % live.size();
            const Live l = live[pick];
            live.erase(live.begin() + static_cast<long>(pick));
            if (!a.free(l.start, l.pages)) {
                std::printf("step %d: free(%u, %u) refused\n", step, l.start, l.pages);
                ok = false;
                break;
            }
            for (uint32_t i = l.start; i < l.start + l.pages; ++i) {
                owner[i] = 0;
            }
        } else if (step < 2000) {
            const uint32_t start = std::rand() % kPages;
            const uint32_t pages = 1 + std::rand() % 16;
            a.reserve(start, pages);
            for (uint32_t i = start; i < start + pages && i < kPages; ++i) {
                if (owner[i] == 0) {
                    owner[i] = -1;
                }
            }
        }
        uint64_t modelFree = 0;
        for (uint32_t i = 0; i < kPages; ++i) {
            modelFree += owner[i] == 0;
        }
        if (modelFree != a.freePages()) {
            std::printf("step %d: model free %llu, allocator %llu\n", step,
                        static_cast<unsigned long long>(modelFree),
                        static_cast<unsigned long long>(a.freePages()));
            ok = false;
        }
    }
    check(ok, "20000 random operations agree with a bitmap model");
}

} // namespace

int main() {
    testDevices();
    testBars();
    testPlane();
    testGgc();
    testPte();
    testCommands();
    testContextImage();
    testPpgtt();
    testRender();
    testAllocatorBasics();
    testAllocatorRandom();
    std::printf("[i915-fixtures] SCORE pass=%d total=%d fail=%d\n", g_pass, g_pass + g_fail, g_fail);
    return g_fail == 0 ? 0 : 1;
}
