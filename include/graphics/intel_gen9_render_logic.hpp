#pragma once

// Pure logic for running real (3D-capable) work on the Gen9 render and
// blitter engines, after Linux's i915 (MIT), kept free of hardware access so
// the host fixtures (tools/i915-fixtures) can check it:
//   * the workarounds i915 applies on Skylake (gt/intel_workarounds.c: the
//     GT, render-engine and context lists, and the register whitelist; plus
//     skl_init_clock_gating's GT part), as tables;
//   * the slice/subslice/EU fuses (gt/intel_sseu.c, gen9_sseu_info_init) and
//     what depends on them: the MCR steering and the IZ hashing tweak;
//   * the null-state ("golden") batch relocated to where it lives
//     (gt/intel_renderstate.c);
//   * the ring commands i915 puts around a batch (gt/gen8_engine_cs.c:
//     flushes and invalidations, the batch start, the breadcrumb) and the
//     context workarounds' MI_LOAD_REGISTER_IMM (intel_engine_emit_ctx_wa);
//   * the register state of a context that runs again (lrc_update_regs).

#include <stdint.h>
#include <graphics/intel_gen9_logic.hpp>
#include <graphics/intel_gen9_renderstate.hpp>

namespace intel_gen9 {

// Commands ------------------------------------------------------------------

constexpr uint32_t MI_STORE_REGISTER_MEM_GEN8 = miInstr(0x24, 2);   // + register, address lo, hi
constexpr uint32_t MI_SRM_LRM_GLOBAL_GTT = 1u << 22;
constexpr uint32_t MI_ARB_ON_OFF = miInstr(0x08, 0);
constexpr uint32_t MI_ARB_ENABLE = 1u << 0;
constexpr uint32_t MI_ARB_DISABLE = 0;
constexpr uint32_t MI_FLUSH_DW_STORE_INDEX = 1u << 21;
constexpr uint32_t MI_INVALIDATE_TLB = 1u << 18;

constexpr uint32_t gfxOpPipeControl(uint32_t length) { return (3u << 29) | (3u << 27) | (2u << 24) | (length - 2); }
constexpr uint32_t PIPE_CONTROL_GLOBAL_GTT_IVB = 1u << 24;
constexpr uint32_t PIPE_CONTROL_STORE_DATA_INDEX = 1u << 21;
constexpr uint32_t PIPE_CONTROL_CS_STALL = 1u << 20;
constexpr uint32_t PIPE_CONTROL_TLB_INVALIDATE = 1u << 18;
constexpr uint32_t PIPE_CONTROL_QW_WRITE = 1u << 14;
constexpr uint32_t PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH = 1u << 12;
constexpr uint32_t PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE = 1u << 11;
constexpr uint32_t PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE = 1u << 10;
constexpr uint32_t PIPE_CONTROL_FLUSH_ENABLE = 1u << 7;
constexpr uint32_t PIPE_CONTROL_DC_FLUSH_ENABLE = 1u << 5;
constexpr uint32_t PIPE_CONTROL_VF_CACHE_INVALIDATE = 1u << 4;
constexpr uint32_t PIPE_CONTROL_CONST_CACHE_INVALIDATE = 1u << 3;
constexpr uint32_t PIPE_CONTROL_STATE_CACHE_INVALIDATE = 1u << 2;
constexpr uint32_t PIPE_CONTROL_DEPTH_CACHE_FLUSH = 1u << 0;

// Context status buffer events (the low dword of an entry).
constexpr uint32_t GEN8_CTX_STATUS_IDLE_ACTIVE = 1u << 0;
constexpr uint32_t GEN8_CTX_STATUS_ACTIVE_IDLE = 1u << 3;   // the engine went idle: the context is saved
constexpr uint32_t GEN8_CTX_STATUS_COMPLETE = 1u << 4;

// The scratch dword in a context's per-process status page (its image's
// first page) that the flushes' post-sync writes go to (STORE_DATA_INDEX).
constexpr uint32_t LRC_PPHWSP_SCRATCH_ADDR = 0x34 * 4;

// Workarounds ---------------------------------------------------------------

// One register setting. A masked register takes `set` as written: the bits
// to change in the upper half, their values in the lower. Any other is read,
// `clear` cleared, `set` set, and written back.
struct Workaround {
    uint32_t reg;
    uint32_t clear;
    uint32_t set;
    bool masked;
};

inline uint32_t workaroundValue(const Workaround& w, uint32_t current) {
    return w.masked ? w.set : ((current & ~w.clear) | w.set);
}

// Whether a read of the register shows the setting (i915's wa_verify).
inline bool workaroundHolds(const Workaround& w, uint32_t readBack) {
    if (w.masked) {
        const uint32_t mask = w.set >> 16;
        return (readBack & mask) == (w.set & mask);
    }
    return (readBack & (w.clear | w.set)) == w.set;
}

// Registers (i915's intel_gt_regs.h).
constexpr uint32_t GEN8_CS_CHICKEN1 = 0x2580;
constexpr uint32_t GEN9_CTX_PREEMPT_REG = 0x2248;
constexpr uint32_t GEN7_FF_SLICE_CS_CHICKEN1 = 0x20e0;
constexpr uint32_t GEN9_CSFE_CHICKEN1_RCS = 0x20d4;
constexpr uint32_t GEN8_MCR_SELECTOR = 0xfdc;
constexpr uint32_t GAM_ECOCHK = 0x4090;
constexpr uint32_t GEN9_GAMT_ECO_REG_RW_IA = 0x4ab0;
constexpr uint32_t MMCD_MISC_CTRL = 0x4ddc;
constexpr uint32_t CACHE_MODE_1 = 0x7004;
constexpr uint32_t GEN7_GT_MODE = 0x7008;
constexpr uint32_t COMMON_SLICE_CHICKEN2 = 0x7014;
constexpr uint32_t HDC_CHICKEN0 = 0x7300;
constexpr uint32_t GEN8_HDC_CHICKEN1 = 0x7304;
constexpr uint32_t FBC_LLC_READ_CTRL = 0x9044;
constexpr uint32_t GEN8_FUSE2 = 0x9120;
constexpr uint32_t GEN9_EU_DISABLE0 = 0x9134;   // + 4 per slice
constexpr uint32_t GEN7_UCGCTL4 = 0x940c;
constexpr uint32_t GEN7_MISCCPCTL = 0x9424;
constexpr uint32_t GEN8_GARBCNTL = 0xb004;
constexpr uint32_t GEN9_SCRATCH_LNCF1 = 0xb008;
constexpr uint32_t GEN8_L3SQCREG4 = 0xb118;
constexpr uint32_t BDW_SCRATCH1 = 0xb11c;        // also GEN9_SCRATCH1
constexpr uint32_t HALF_SLICE_CHICKEN2 = 0xe180;
constexpr uint32_t GEN8_HALF_SLICE_CHICKEN3 = 0xe184;
constexpr uint32_t GEN9_HALF_SLICE_CHICKEN5 = 0xe188;
constexpr uint32_t GEN9_HALF_SLICE_CHICKEN7 = 0xe194;
constexpr uint32_t GEN8_ROW_CHICKEN = 0xe4f0;

// The render context's own settings (skl_ctx_workarounds_init, gen9's list
// for an LLC part), written by MI_LOAD_REGISTER_IMM in each new context; the
// IZ hashing tweak (skl_tune_iz_hashing) comes on top when the fuses call for
// it. Entries for one register are merged, as i915's wa_add does.
constexpr Workaround kSklContextWorkarounds[] = {
    // WaDisable3DMidCmdPreemption + WaDisableGPGPUMidCmdPreemption: object
    // level preemption off, GPGPU at command level.
    {GEN8_CS_CHICKEN1, 0, (0x7u << 16) | 0x2u, true},
    // Wa4x4STCOptimizationDisable, WaDisablePartialResolveInVc.
    {CACHE_MODE_1, 0, maskedEnable((1u << 6) | (1u << 1)), true},
    // WaCompressedResourceSamplerPbeMediaNewHashMode (LLC parts).
    {COMMON_SLICE_CHICKEN2, 0, maskedEnable(1u << 13), true},
    // WaForceContextSaveRestoreNonCoherent, WaForceEnableNonCoherent.
    {HDC_CHICKEN0, 0, maskedEnable((1u << 15) | (1u << 5) | (1u << 4)), true},
    // WaDisableSTUnitPowerOptimization.
    {HALF_SLICE_CHICKEN2, 0, maskedEnable(1u << 13), true},
    // WaDisableSamplerPowerBypassForSOPingPong.
    {GEN8_HALF_SLICE_CHICKEN3, 0, maskedEnable(1u << 1), true},
    // WaCcsTlbPrefetchDisable.
    {GEN9_HALF_SLICE_CHICKEN5, 0, maskedDisable(1u << 3), true},
    // WaCompressedResourceSamplerPbeMediaNewHashMode, WaEnableYV12BugFix,
    // WaEnableSamplerGPGPUPreemptionSupport.
    {GEN9_HALF_SLICE_CHICKEN7, 0, maskedEnable((1u << 8) | (1u << 4) | (1u << 2)), true},
    // WaClearFlowControlGpgpuContextSave, WaDisablePartialInstShootdown.
    {GEN8_ROW_CHICKEN, 0, maskedEnable((1u << 15) | (1u << 8)), true},
};
constexpr uint32_t kSklContextWorkaroundCount = sizeof(kSklContextWorkarounds) / sizeof(kSklContextWorkarounds[0]);

// GT-wide settings (gen9_gt_workarounds_init + skl_gt_workarounds_init, and
// skl_init_clock_gating's two GT registers), written once by MMIO. The MCR
// steering (first) and WaInPlaceDecompressionHang (steppings before H0) are
// added by gen9GtWorkarounds.
constexpr Workaround kSklGtWorkarounds[] = {
    // WaDisableKillLogic, WaDisableHDCInvalidation.
    {GAM_ECOCHK, 0, (1u << 8) | (1u << 25), false},
    // WaCompressedResourceSamplerPbeMediaNewHashMode (LLC parts).
    {MMCD_MISC_CTRL, 0, (1u << 31) | (1u << 27), false},
    // WaDisableGafsUnitClkGating.
    {GEN7_UCGCTL4, 0, 1u << 14, false},
    // WaDisableDopClockGating (clock gating init).
    {GEN7_MISCCPCTL, 1u << 0, 0, false},
    // WAC6entrylatency (clock gating init).
    {FBC_LLC_READ_CTRL, 0, 1u << 30, false},
};
constexpr uint32_t kSklGtWorkaroundCount = sizeof(kSklGtWorkarounds) / sizeof(kSklGtWorkarounds[0]);

// The render engine's registers (rcs_engine_wa_init for Gen9 and Skylake),
// written by MMIO after every engine reset.
constexpr Workaround kSklRenderWorkarounds[] = {
    // Per-context preemption control (i915's "Option 2", gen9+).
    {GEN7_FF_SLICE_CS_CHICKEN1, 0, maskedEnable(1u << 14), true},
    // WaContextSwitchWithConcurrentTLBInvalidate.
    {GEN9_CSFE_CHICKEN1_RCS, 0, maskedEnable(1u << 2), true},
    // WaEnableGapsTsvCreditFix.
    {GEN8_GARBCNTL, 0, 1u << 7, false},
    // L3 atomics off (unrecoverable hangs otherwise).
    {GEN9_SCRATCH_LNCF1, 1u << 0, 0, false},
    // WaOCLCoherentLineFlush; L3 atomics off.
    {GEN8_L3SQCREG4, 1u << 22, 1u << 21, false},
    // WaEnableLbsSlaRetryTimerDecrement; EVICTION_PERF_FIX_ENABLE off.
    {BDW_SCRATCH1, 1u << 8, 1u << 2, false},
};
constexpr uint32_t kSklRenderWorkaroundCount = sizeof(kSklRenderWorkarounds) / sizeof(kSklRenderWorkarounds[0]);

// Registers a non-privileged (user) batch may write on the render engine
// (gen9_whitelist_build + skl_whitelist_build), through the engine's
// RING_FORCE_TO_NONPRIV slots; the unused slots point at RING_NOPID.
constexpr uint32_t kSklRenderWhitelist[] = {
    GEN9_CTX_PREEMPT_REG,    // WaVFEStateAfterPipeControlwithMediaStateClear
    GEN8_CS_CHICKEN1,        // WaEnablePreemptionGranularityControlByUMD
    GEN8_HDC_CHICKEN1,       // WaAllowUMDToModifyHDCChicken1
    COMMON_SLICE_CHICKEN2,   // WaSendPushConstantsFromMMIO
    GEN8_L3SQCREG4,          // WaDisableLSQCROPERFforOCL
};
constexpr uint32_t kSklRenderWhitelistCount = sizeof(kSklRenderWhitelist) / sizeof(kSklRenderWhitelist[0]);
constexpr uint32_t RING_MAX_NONPRIV_SLOTS = 12;
constexpr uint32_t ringForceToNonpriv(uint32_t base, uint32_t slot) { return base + 0x4d0 + slot * 4; }
constexpr uint32_t ringNopid(uint32_t base) { return base + 0x94; }

// Fuses --------------------------------------------------------------------

// What GEN8_FUSE2 and GEN9_EU_DISABLE(slice) say about the EUs: Skylake
// has up to 3 slices of 4 subslices of 8 EUs.
struct Gen9Sseu {
    uint8_t sliceMask;
    uint8_t subsliceMask;          // the same for every enabled slice
    uint8_t eus[3][4];             // EUs per subslice (0: subslice fused off)
    uint8_t euMask[3][4];          // which of its 8 EUs are there
    uint8_t subslice7eu[3];        // subslices with exactly 7 EUs, per slice
    uint32_t euTotal;
};

inline uint32_t popcount8(uint32_t v) {
    uint32_t n = 0;
    for (; v; v &= v - 1) {
        ++n;
    }
    return n;
}

inline uint32_t lowestBit(uint32_t mask) {
    for (uint32_t i = 0; i < 32; ++i) {
        if (mask & (1u << i)) {
            return i;
        }
    }
    return 32;
}

inline Gen9Sseu decodeGen9Sseu(uint32_t fuse2, const uint32_t euDisable[3]) {
    Gen9Sseu s = {};
    s.sliceMask = static_cast<uint8_t>((fuse2 >> 25) & 0x7);                 // GEN8_F2_S_ENA
    s.subsliceMask = static_cast<uint8_t>(0xF & ~((fuse2 >> 20) & 0xF));      // GEN9_F2_SS_DIS
    for (uint32_t slice = 0; slice < 3; ++slice) {
        if (!(s.sliceMask & (1u << slice))) {
            continue;
        }
        for (uint32_t ss = 0; ss < 4; ++ss) {
            if (!(s.subsliceMask & (1u << ss))) {
                continue;
            }
            const uint32_t disabled = (euDisable[slice] >> (ss * 8)) & 0xFF;
            const uint32_t eus = 8 - popcount8(disabled);
            s.eus[slice][ss] = static_cast<uint8_t>(eus);
            s.euMask[slice][ss] = static_cast<uint8_t>(~disabled & 0xFF);
            s.euTotal += eus;
            if (eus == 7) {
                s.subslice7eu[slice] |= static_cast<uint8_t>(1u << ss);
            }
        }
    }
    return s;
}

// DRM_I915_QUERY_TOPOLOGY_INFO as i915's query_topology_info lays it out
// for a big-core Gen9 part (gen9_sseu_info_init: 3 slices of 4 subslices of
// 8 EUs at most): a 16-byte header, the slice mask, a subslice mask per
// slice, an EU mask per subslice. Returns the bytes needed; writes them
// only when `out` has room.
constexpr uint32_t kGen9MaxSlices = 3;
constexpr uint32_t kGen9MaxSubslices = 4;
constexpr uint32_t kGen9MaxEusPerSubslice = 8;
constexpr uint32_t kGen9TopologyBytes = 16 + 1 + kGen9MaxSlices + kGen9MaxSlices * kGen9MaxSubslices;

inline uint32_t encodeGen9Topology(const Gen9Sseu& s, uint8_t* out, uint32_t room) {
    if (!out || room < kGen9TopologyBytes) {
        return kGen9TopologyBytes;
    }
    for (uint32_t i = 0; i < kGen9TopologyBytes; ++i) {
        out[i] = 0;
    }
    const uint16_t header[8] = {
        0,                                     // flags
        kGen9MaxSlices, kGen9MaxSubslices, kGen9MaxEusPerSubslice,
        1,                                     // subslice_offset: after the slice mask byte
        1,                                     // subslice_stride: 4 subslices fit a byte
        static_cast<uint16_t>(1 + kGen9MaxSlices),   // eu_offset
        1,                                     // eu_stride: 8 EUs fit a byte
    };
    for (uint32_t i = 0; i < 8; ++i) {
        out[i * 2] = static_cast<uint8_t>(header[i]);
        out[i * 2 + 1] = static_cast<uint8_t>(header[i] >> 8);
    }
    uint8_t* data = out + 16;
    data[0] = s.sliceMask;
    for (uint32_t slice = 0; slice < kGen9MaxSlices; ++slice) {
        if (!(s.sliceMask & (1u << slice))) {
            continue;
        }
        data[1 + slice] = s.subsliceMask;
        for (uint32_t ss = 0; ss < kGen9MaxSubslices; ++ss) {
            if (s.subsliceMask & (1u << ss)) {
                data[1 + kGen9MaxSlices + slice * kGen9MaxSubslices + ss] = s.euMask[slice][ss];
            }
        }
    }
    return kGen9TopologyBytes;
}

// WaProgramMgsrForCorrectSliceSpecificMmioReads: reads of slice/subslice
// registers go to the first enabled slice and subslice.
inline Workaround gen9McrSteering(const Gen9Sseu& s) {
    const uint32_t slice = lowestBit(s.sliceMask) & 3;
    const uint32_t subslice = lowestBit(s.subsliceMask) & 3;
    return {GEN8_MCR_SELECTOR, (3u << 26) | (3u << 24), (slice << 26) | (subslice << 24), false};
}

// skl_tune_iz_hashing: in a slice where exactly one subslice has 7 EUs, the
// hash favours the others. 0 when nothing needs tuning, else the masked
// value for GEN7_GT_MODE.
inline uint32_t gen9IzHashing(const Gen9Sseu& s) {
    uint32_t vals[3] = {0, 0, 0};
    for (uint32_t slice = 0; slice < 3; ++slice) {
        const uint32_t m = s.subslice7eu[slice];
        if (m == 0 || (m & (m - 1)) != 0) {
            continue;   // not exactly one
        }
        vals[slice] = 3 - lowestBit(m);
    }
    if (vals[0] == 0 && vals[1] == 0 && vals[2] == 0) {
        return 0;
    }
    return (0x3Fu << 16) | (vals[2] << 4) | (vals[1] << 2) | vals[0];
}

// The render context's R_PWR_CLK_STATE (intel_sseu_make_rpcs with the
// device's own configuration): with power gating the hardware may leave
// slices or EUs off unless the context asks for them. Skylake gates slices
// when it has more than one, and EUs when a subslice has more than 2; the
// request is every slice and all 8 EUs of a subslice.
constexpr uint32_t GEN8_RPCS_ENABLE = 1u << 31;
constexpr uint32_t GEN8_RPCS_S_CNT_ENABLE = 1u << 18;
inline uint32_t gen9Rpcs(const Gen9Sseu& s) {
    uint32_t rpcs = 0;
    const uint32_t slices = popcount8(s.sliceMask);
    if (slices > 1) {
        rpcs |= GEN8_RPCS_ENABLE | GEN8_RPCS_S_CNT_ENABLE | (slices << 15);
    }
    const uint32_t subslices = slices * popcount8(s.subsliceMask);
    const uint32_t euPerSubslice = subslices ? (s.euTotal + subslices - 1) / subslices : 0;
    if (euPerSubslice > 2) {
        rpcs |= GEN8_RPCS_ENABLE | (8u << 4) | 8u;   // EU max, EU min
    }
    return rpcs;
}

// Steppings: Skylake's PCI revision ids, A0 = 0 ... H0 = 7 (i915's skl_revids).
constexpr uint8_t SKL_REVID_H0 = 7;

// The GT list for this part: MCR steering first, then kSklGtWorkarounds, and
// WaInPlaceDecompressionHang before H0. Returns the count (at most
// kSklGtWorkaroundCount + 2).
inline uint32_t gen9GtWorkarounds(Workaround* out, const Gen9Sseu& s, uint8_t revision) {
    uint32_t n = 0;
    out[n++] = gen9McrSteering(s);
    for (uint32_t i = 0; i < kSklGtWorkaroundCount; ++i) {
        out[n++] = kSklGtWorkarounds[i];
    }
    if (revision < SKL_REVID_H0) {
        out[n++] = {GEN9_GAMT_ECO_REG_RW_IA, 0, 1u << 18, false};
    }
    return n;
}

// The context list: kSklContextWorkarounds, plus the IZ hashing when needed.
inline uint32_t gen9ContextWorkarounds(Workaround* out, const Gen9Sseu& s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kSklContextWorkaroundCount; ++i) {
        out[n++] = kSklContextWorkarounds[i];
    }
    const uint32_t iz = gen9IzHashing(s);
    if (iz != 0) {
        out[n++] = {GEN7_GT_MODE, 0, iz, true};
    }
    return n;
}

// The null-state batch -------------------------------------------------------

// render_state_setup: the batch with each relocated qword pointing into its
// own copy at `ggtt`. False if the table does not fit the batch.
inline bool relocateNullState(uint32_t* out, uint64_t ggtt) {
    uint32_t reloc = 0;
    uint32_t i = 0;
    while (i < kGen9NullStateDwords) {
        const uint32_t dword = kGen9NullStateBatch[i];
        if (reloc < kGen9NullStateRelocCount && i * 4 == kGen9NullStateRelocs[reloc]) {
            if (i + 1 >= kGen9NullStateDwords || kGen9NullStateBatch[i + 1] != 0) {
                return false;
            }
            const uint64_t address = dword + ggtt;
            out[i++] = static_cast<uint32_t>(address);
            out[i++] = static_cast<uint32_t>(address >> 32);
            ++reloc;
            continue;
        }
        out[i++] = dword;
    }
    return reloc == kGen9NullStateRelocCount;
}

// Ring commands (gen8_engine_cs.c) ------------------------------------------

inline uint32_t* emitPipeControl(uint32_t* cs, uint32_t flags, uint32_t offset) {
    cs[0] = gfxOpPipeControl(6);
    cs[1] = flags;
    cs[2] = offset;
    cs[3] = 0;
    cs[4] = 0;
    cs[5] = 0;
    return cs + 6;
}

// gen8_emit_flush_rcs: `flush` writes the render caches back, `invalidate`
// drops the GPU's TLBs and read caches (with Gen9's null PIPE_CONTROL before
// a VF cache invalidation). The post-sync write goes to the scratch dword.
inline uint32_t* emitFlushRcs(uint32_t* cs, bool flush, bool invalidate) {
    uint32_t flags = PIPE_CONTROL_CS_STALL;
    if (flush) {
        flags |= PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                 PIPE_CONTROL_DC_FLUSH_ENABLE | PIPE_CONTROL_FLUSH_ENABLE;
    }
    if (invalidate) {
        flags |= PIPE_CONTROL_TLB_INVALIDATE | PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE |
                 PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE | PIPE_CONTROL_VF_CACHE_INVALIDATE |
                 PIPE_CONTROL_CONST_CACHE_INVALIDATE | PIPE_CONTROL_STATE_CACHE_INVALIDATE |
                 PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_STORE_DATA_INDEX;
        cs = emitPipeControl(cs, 0, 0);
    }
    return emitPipeControl(cs, flags, LRC_PPHWSP_SCRATCH_ADDR);
}

// gen8_emit_flush_xcs: MI_FLUSH_DW with a post-sync store (a barrier), and
// the TLB invalidation.
inline uint32_t* emitFlushXcs(uint32_t* cs, bool invalidate) {
    cs[0] = (MI_FLUSH_DW + 1) | MI_FLUSH_DW_STORE_INDEX | MI_FLUSH_DW_OP_STOREDW |
            (invalidate ? MI_INVALIDATE_TLB : 0);
    cs[1] = LRC_PPHWSP_SCRATCH_ADDR;
    cs[2] = 0;
    cs[3] = 0;
    return cs + 4;
}

// gen8_emit_bb_start: arbitration on around the batch. `ppgtt` makes it a
// non-privileged batch from the context's address space; without, a
// privileged one from the GGTT.
inline uint32_t* emitBatchStart(uint32_t* cs, uint64_t address, bool ppgtt) {
    cs[0] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
    cs[1] = MI_BATCH_BUFFER_START_GEN8 | (ppgtt ? MI_BATCH_PPGTT : 0);
    cs[2] = static_cast<uint32_t>(address);
    cs[3] = static_cast<uint32_t>(address >> 32);
    cs[4] = MI_ARB_ON_OFF | MI_ARB_DISABLE;
    cs[5] = MI_NOOP;
    return cs + 6;
}

// The breadcrumb (gen8_emit_fini_breadcrumb_rcs/xcs): flush, then store
// `seqno` at `ggtt` (8-byte aligned, bit 5 clear for MI_FLUSH_DW) once
// everything before is done; arbitration on again.
inline uint32_t* emitBreadcrumb(uint32_t* cs, bool render, uint32_t seqno, uint32_t ggtt) {
    if (render) {
        // gen8_emit_fini_breadcrumb_rcs: the store of the sequence number is
        // the PIPE_CONTROL that flushes the render target, depth and data
        // port (L3) caches, so it lands only once what the batch drew is in
        // memory; then a flush with a CS stall. (A flush PIPE_CONTROL that
        // also invalidated the TLBs without a post-sync operation, as this
        // was, is not a valid command: the batch's writes stayed in the GPU's
        // caches, where only the GPU saw them.)
        cs[0] = gfxOpPipeControl(6);
        cs[1] = PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                PIPE_CONTROL_DC_FLUSH_ENABLE | PIPE_CONTROL_GLOBAL_GTT_IVB | PIPE_CONTROL_QW_WRITE;
        cs[2] = ggtt;
        cs[3] = 0;
        cs[4] = seqno;
        cs[5] = 0;
        cs += 6;
        cs = emitPipeControl(cs, PIPE_CONTROL_FLUSH_ENABLE | PIPE_CONTROL_CS_STALL, 0);
    } else {
        cs[0] = (MI_FLUSH_DW + 1) | MI_FLUSH_DW_OP_STOREDW;
        cs[1] = ggtt | MI_FLUSH_DW_USE_GTT;
        cs[2] = 0;
        cs[3] = seqno;
        cs += 4;
    }
    cs[0] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
    cs[1] = MI_NOOP;
    return cs + 2;
}

// intel_engine_emit_ctx_wa: one MI_LOAD_REGISTER_IMM with every setting.
inline uint32_t* emitContextWorkarounds(uint32_t* cs, const Workaround* list, uint32_t count) {
    if (count == 0) {
        return cs;
    }
    *cs++ = miLoadRegisterImm(count);
    for (uint32_t i = 0; i < count; ++i) {
        *cs++ = list[i].reg;
        *cs++ = list[i].set;
    }
    *cs++ = MI_NOOP;
    return cs;
}

// MI_STORE_REGISTER_MEM of `reg` to `ggtt` (from the ring: privileged).
inline uint32_t* emitStoreRegisterGgtt(uint32_t* cs, uint32_t reg, uint32_t ggtt) {
    cs[0] = MI_STORE_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT;
    cs[1] = reg;
    cs[2] = ggtt;
    cs[3] = 0;
    return cs + 4;
}

// The render engine's indirect context batch (i915's gen9_init_indirectctx_bb
// in gt/intel_lrc.c), which the engine runs each time it restores a render
// context. WaFlushCoherentL3CacheLinesAtContextSwitch: a data-port flush
// writes out only the non-coherent lines of the L3 unless L3SQCREG4 asks for
// the coherent ones too, so the register is saved to `scratchGgtt`, set for
// one flush and loaded back (gen8_emit_flush_coherentl3_wa).
// WaClearSlmSpaceAtContextSwitch, then three provoking-vertex and gather
// settings by LRI. The batch is padded to a cache line, the unit the context
// register gives its size in.
constexpr uint32_t MI_LOAD_REGISTER_MEM_GEN8 = miInstr(0x29, 2);   // + register, address lo, hi
constexpr uint32_t PIPE_CONTROL_FLUSH_L3 = 1u << 27;
constexpr uint32_t GEN8_LQSC_FLUSH_COHERENT_LINES = 1u << 21;
constexpr uint32_t FF_SLICE_CHICKEN = 0x2088;
constexpr uint32_t FF_SLICE_CHICKEN_CL_PROVOKING_VERTEX_FIX = 1u << 1;
constexpr uint32_t CHICKEN3_3D = 0x2090;
constexpr uint32_t CHICKEN3_3D_SF_PROVOKING_VERTEX_FIX = 1u << 12;
constexpr uint32_t GEN9_DISABLE_GATHER_AT_SET_SHADER_COMMON_SLICE = 1u << 12;
constexpr uint32_t GEN9_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT = 0x26;
constexpr uint32_t CTX_BB_PER_CTX_PTR = 0x18 + 1;      // lrc_ring_wa_bb_per_ctx (Gen9): the value dwords
constexpr uint32_t CTX_INDIRECT_CTX = 0x1a + 1;
constexpr uint32_t CTX_INDIRECT_CTX_OFFSET = 0x1c + 1;
constexpr uint32_t kCacheLineBytes = 64;

inline uint32_t* emitGen9IndirectCtx(uint32_t* start, uint32_t scratchGgtt) {
    uint32_t* cs = start;
    *cs++ = MI_ARB_ON_OFF | MI_ARB_DISABLE;

    *cs++ = MI_STORE_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT;
    *cs++ = GEN8_L3SQCREG4;
    *cs++ = scratchGgtt;
    *cs++ = 0;
    *cs++ = miLoadRegisterImm(1);
    *cs++ = GEN8_L3SQCREG4;
    *cs++ = 0x40400000u | GEN8_LQSC_FLUSH_COHERENT_LINES;
    cs = emitPipeControl(cs, PIPE_CONTROL_CS_STALL | PIPE_CONTROL_DC_FLUSH_ENABLE, 0);
    *cs++ = MI_LOAD_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT;
    *cs++ = GEN8_L3SQCREG4;
    *cs++ = scratchGgtt;
    *cs++ = 0;

    cs = emitPipeControl(cs, PIPE_CONTROL_FLUSH_L3 | PIPE_CONTROL_STORE_DATA_INDEX | PIPE_CONTROL_CS_STALL |
                                 PIPE_CONTROL_QW_WRITE,
                         LRC_PPHWSP_SCRATCH_ADDR);

    *cs++ = miLoadRegisterImm(3);
    *cs++ = COMMON_SLICE_CHICKEN2;
    *cs++ = GEN9_DISABLE_GATHER_AT_SET_SHADER_COMMON_SLICE << 16;
    *cs++ = FF_SLICE_CHICKEN;
    *cs++ = (FF_SLICE_CHICKEN_CL_PROVOKING_VERTEX_FIX << 16) | FF_SLICE_CHICKEN_CL_PROVOKING_VERTEX_FIX;
    *cs++ = CHICKEN3_3D;
    *cs++ = (CHICKEN3_3D_SF_PROVOKING_VERTEX_FIX << 16) | CHICKEN3_3D_SF_PROVOKING_VERTEX_FIX;
    *cs++ = MI_NOOP;

    *cs++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;
    while (((cs - start) * 4) % kCacheLineBytes != 0) {
        *cs++ = MI_NOOP;
    }
    return cs;
}

// i915's lrc_setup_indirect_ctx: where the batch is and how many cache lines
// it has, and the offset into the restore at which the engine runs it.
inline void setIndirectCtx(uint32_t* regs, uint32_t ggtt, uint32_t bytes) {
    regs[CTX_INDIRECT_CTX] = ggtt | (bytes / kCacheLineBytes);
    regs[CTX_INDIRECT_CTX_OFFSET] = GEN9_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT << 6;
}

// Persistent contexts ----------------------------------------------------------

// The register state of a context that ran before, for its next run: the
// engine restores what it saved; the ring window and the context control are
// set again (i915's lrc_update_regs, and __lrc_init_regs without inhibit).
inline void prepareContextResubmit(uint32_t* regs, uint32_t head, uint32_t tail) {
    regs[CTX_CONTEXT_CONTROL] = maskedEnable(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH) |
                                maskedDisable(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT) |
                                maskedDisable(CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT | CTX_CTRL_RS_CTX_ENABLE);
    regs[CTX_RING_HEAD] = head;
    regs[CTX_RING_TAIL] = tail;
}

// Where `bytes` of commands go in a ring of `ringBytes` whose free space
// starts at `tail` (the ring is idle: execution is synchronous): at `tail`,
// or at 0 when they would run past the end, which the caller fills with
// MI_NOOP from `tail` (the engine runs through the padding and wraps).
inline uint32_t ringPlacement(uint32_t tail, uint32_t bytes, uint32_t ringBytes) {
    return tail + bytes <= ringBytes ? tail : 0;
}

// Memory object control state (i915's intel_mocs.c, skl_mocs_table): what
// each MOCS index in a surface or base-address command means for caching.
// Mesa asks for index 2 (write-back in the LLC and eLLC) for its own buffers
// and index 1 (as the page table says) for shared ones; unprogrammed, the
// indexes hold the hardware's reset values. Entries Mesa does not use get
// index 1's, as i915 sets them.
constexpr uint32_t MOCS_ENTRIES = 62;               // GEN9_NUM_MOCS_ENTRIES
constexpr uint32_t GEN9_RCS_MOCS_BASE = 0xC800;     // GEN9_GFX_MOCS(0); context-saved
constexpr uint32_t GEN9_BCS_MOCS_BASE = 0xCC00;     // GEN9_BLT_MOCS(0)
constexpr uint32_t GEN9_LNCFCMOCS_BASE = 0xB020;    // two 16-bit L3 entries a register

// LE_CACHEABILITY (bits 1:0: 0 page table, 1 UC, 3 WB), LE_TGT_CACHE (3:2:
// 0 page table, 2 LLC+eLLC), LE_LRUM (5:4); L3 cacheability in bits 5:4.
inline uint32_t mocsControl(uint32_t index) {
    switch (index) {
    case 0: return 0x1 | 2 << 2;            // I915_MOCS_UNCACHED: LE_1_UC | LE_TC_2_LLC_ELLC
    case 2: return 0x3 | 2 << 2 | 3 << 4;   // I915_MOCS_CACHED: LE_3_WB | LE_TC_2_LLC_ELLC | LE_LRUM(3)
    default: return 3 << 4;                 // I915_MOCS_PTE: LE_0_PAGETABLE | LE_TC_0_PAGETABLE | LE_LRUM(3)
    }
}
inline uint32_t mocsL3(uint32_t index) {
    return index == 0 ? 1u << 4 : 3u << 4;   // L3_1_UC, else L3_3_WB
}
inline uint32_t mocsL3Register(uint32_t pair) {
    return mocsL3(2 * pair) | mocsL3(2 * pair + 1) << 16;
}

// One MI_LOAD_REGISTER_IMM of an engine's MOCS control table and the L3
// table (i915's emit_mocs_control_table and emit_mocs_l3cc_table).
inline uint32_t* emitMocsTables(uint32_t* cs, uint32_t engineMocsBase) {
    *cs++ = miLoadRegisterImm(MOCS_ENTRIES);
    for (uint32_t i = 0; i < MOCS_ENTRIES; ++i) {
        *cs++ = engineMocsBase + i * 4;
        *cs++ = mocsControl(i);
    }
    *cs++ = miLoadRegisterImm(MOCS_ENTRIES / 2);
    for (uint32_t i = 0; i < MOCS_ENTRIES / 2; ++i) {
        *cs++ = GEN9_LNCFCMOCS_BASE + i * 4;
        *cs++ = mocsL3Register(i);
    }
    *cs++ = MI_NOOP;
    return cs;
}

} // namespace intel_gen9
