#pragma once

// The Intel Gen9 (Skylake) driver's pure logic: no hardware access and no
// kernel dependencies, so the host fixtures (tools/i915-fixtures) compile it
// as is. The driver (intel_gen9.hpp, intel_gen9_gtt.cpp) applies it to the
// hardware. Register meanings follow Linux's i915 (i915_reg.h, intel_gtt.c,
// i915_gem_stolen.c) and the Skylake PRM.

#include <stddef.h>
#include <stdint.h>

namespace intel_gen9 {

// PCI identity ---------------------------------------------------------------

constexpr uint16_t INTEL_VENDOR_ID = 0x8086;

enum class Gen9Gt : uint8_t {
    Unknown = 0,
    Gt1,
    Gt1_5,
    Gt2,   // HD Graphics 530 is a GT2 part.
    Gt3,
    Gt3e,
    Gt4,
    Gt4e,
};

struct Gen9DeviceInfo {
    uint16_t deviceId;
    Gen9Gt gt;
    bool isHd530;     // true for the specific HD Graphics 530 SKUs.
    const char* name; // Human readable marketing/codename string.
};

// The Skylake (Gen9) VGA device ids, from i915_pciids.h's INTEL_SKL_*_IDS.
// HD Graphics 530 is the GT2 desktop part 0x1912 and the GT2 halo part 0x191B.
inline constexpr Gen9DeviceInfo kGen9Devices[] = {
    // GT1
    {0x1906, Gen9Gt::Gt1,   false, "Skylake GT1 (ULT)"},
    {0x190E, Gen9Gt::Gt1,   false, "Skylake GT1 (ULX)"},
    {0x1902, Gen9Gt::Gt1,   false, "Skylake GT1 (Desktop)"},
    {0x190A, Gen9Gt::Gt1,   false, "Skylake GT1 (Server)"},
    {0x190B, Gen9Gt::Gt1,   false, "Skylake GT1 (Halo)"},
    // GT1.5
    {0x1913, Gen9Gt::Gt1_5, false, "Skylake GT1.5 (ULT)"},
    {0x1915, Gen9Gt::Gt1_5, false, "Skylake GT1.5 (ULX)"},
    {0x1917, Gen9Gt::Gt1_5, false, "Skylake GT1.5 (Desktop)"},
    // GT2 - HD Graphics 530 / 520 family
    {0x1916, Gen9Gt::Gt2,   false, "HD Graphics 520 (Skylake GT2 ULT)"},
    {0x1921, Gen9Gt::Gt2,   false, "Skylake GT2F (ULT)"},
    {0x191E, Gen9Gt::Gt2,   false, "HD Graphics 515 (Skylake GT2 ULX)"},
    {0x1912, Gen9Gt::Gt2,   true,  "HD Graphics 530 (Skylake GT2 Desktop)"},
    {0x191A, Gen9Gt::Gt2,   false, "Skylake GT2 (Server)"},
    {0x191B, Gen9Gt::Gt2,   true,  "HD Graphics 530 (Skylake GT2 Mobile/Halo)"},
    {0x191D, Gen9Gt::Gt2,   false, "HD Graphics P530 (Skylake GT2 Workstation)"},
    // GT3 / GT3e - Iris 540/550
    {0x1923, Gen9Gt::Gt3,   false, "HD Graphics 535 (Skylake GT3 ULT)"},
    {0x1926, Gen9Gt::Gt3e,  false, "Iris Graphics 540 (Skylake GT3e ULT)"},
    {0x1927, Gen9Gt::Gt3e,  false, "Iris Graphics 550 (Skylake GT3e ULT)"},
    {0x192A, Gen9Gt::Gt3,   false, "Skylake GT3 (Server)"},
    {0x192B, Gen9Gt::Gt3e,  false, "Iris Graphics (Skylake GT3e Halo)"},
    {0x192D, Gen9Gt::Gt3e,  false, "Skylake GT3e (Server)"},
    // GT4 / GT4e - Iris Pro 580
    {0x1932, Gen9Gt::Gt4,   false, "Iris Pro 580 (Skylake GT4 Desktop)"},
    {0x193A, Gen9Gt::Gt4e,  false, "Iris Pro P580 (Skylake GT4e Server)"},
    {0x193B, Gen9Gt::Gt4e,  false, "Iris Pro 580 (Skylake GT4e Halo)"},
    {0x193D, Gen9Gt::Gt4e,  false, "Iris Pro P580 (Skylake GT4e Workstation)"},
};

// The matching Skylake/Gen9 descriptor, or nullptr if (vendor, device) is not
// a supported Gen9 part. This is the single source of truth for "do we drive it".
inline const Gen9DeviceInfo* lookupDevice(uint16_t vendorId, uint16_t deviceId) {
    if (vendorId != INTEL_VENDOR_ID) {
        return nullptr;
    }
    for (const Gen9DeviceInfo& dev : kGen9Devices) {
        if (dev.deviceId == deviceId) {
            return &dev;
        }
    }
    return nullptr;
}

// True when the device id is one of the HD Graphics 530 SKUs (0x1912/0x191B).
inline bool isHd530(uint16_t deviceId) {
    const Gen9DeviceInfo* dev = lookupDevice(INTEL_VENDOR_ID, deviceId);
    return dev != nullptr && dev->isHd530;
}

// A 64-bit memory BAR pair as a base address; 0 for an I/O BAR or a disabled
// or unmapped one.
inline uint64_t decodeMemoryBar(uint32_t barLow, uint32_t barHigh) {
    if (barLow == 0 || barLow == 0xFFFFFFFFu) {
        return 0;
    }
    if (barLow & 0x1) {
        return 0;   // I/O space
    }
    uint64_t base = static_cast<uint64_t>(barLow & ~0xFu);
    const uint32_t type = (barLow >> 1) & 0x3;
    if (type == 0x2) {
        base |= static_cast<uint64_t>(barHigh) << 32;
    }
    return base;
}

// Display plane ---------------------------------------------------------------

// PLANE_CTL bit fields used to interpret the firmware-programmed plane.
constexpr uint32_t PLANE_CTL_ENABLE = 1u << 31;
constexpr uint32_t PLANE_CTL_FORMAT_MASK = 0xfu << 24;
constexpr uint32_t PLANE_CTL_FORMAT_XRGB8888 = 0x4u << 24; // 8:8:8:8 (BGRX/XRGB).
constexpr uint32_t PLANE_CTL_TILED_MASK = 0x7u << 10;
constexpr uint32_t PLANE_CTL_TILED_LINEAR = 0x0u << 10;

// PLANE_STRIDE on Gen9 is expressed in units of 64 bytes.
constexpr uint32_t PLANE_STRIDE_UNIT = 64;

// A linear scanout surface must start on 256 KiB in the GGTT (i915's
// intel_surf_alignment for display version 9).
constexpr uint64_t PLANE_SURF_ALIGN = 256 * 1024;

struct DisplayMode {
    uint32_t width;       // active pixels per scanline.
    uint32_t height;      // active scanlines.
    uint32_t stride;      // bytes per scanline.
    uint32_t bpp;         // bits per pixel (32 for XRGB8888).
    uint64_t surfaceGtt;  // PLANE_SURF value: offset into GMADR/GTT space.
    bool tiledLinear;     // true if the firmware left the plane in linear tiling.
    bool valid;           // true if a usable, enabled plane was recovered.
};

// Raw display register values as a DisplayMode. fallbackW/H/stride are used
// when the plane is disabled, tiled or its size reads back as zero.
inline DisplayMode interpretPlane(uint32_t planeCtl, uint32_t planeStride,
                                  uint32_t pipeSrcsz, uint32_t planeSurf,
                                  uint32_t fallbackW, uint32_t fallbackH,
                                  uint32_t fallbackStride) {
    DisplayMode m = {};
    m.bpp = 32;
    m.surfaceGtt = static_cast<uint64_t>(planeSurf & ~0xFFFu);

    const bool enabled = (planeCtl & PLANE_CTL_ENABLE) != 0;
    const uint32_t tiled = planeCtl & PLANE_CTL_TILED_MASK;
    m.tiledLinear = (tiled == PLANE_CTL_TILED_LINEAR);

    // PIPE_SRCSZ encodes (width-1)<<16 | (height-1).
    const uint32_t srcW = ((pipeSrcsz >> 16) & 0x1FFF) + 1;
    const uint32_t srcH = (pipeSrcsz & 0x1FFF) + 1;

    if (enabled && srcW > 1 && srcH > 1 && srcW <= 8192 && srcH <= 8192) {
        m.width = srcW;
        m.height = srcH;
        const uint32_t strideBytes = (planeStride & 0x3FF) * PLANE_STRIDE_UNIT;
        m.stride = (strideBytes >= srcW * 4) ? strideBytes : srcW * 4;
        m.valid = m.tiledLinear; // only the linear case is safe to scan out as-is.
    }

    if (!m.valid) {
        m.width = fallbackW;
        m.height = fallbackH;
        m.stride = (fallbackStride != 0) ? fallbackStride : fallbackW * 4;
        m.tiledLinear = true;
        m.valid = (fallbackW > 0 && fallbackH > 0);
    }
    return m;
}

// Global GTT -------------------------------------------------------------------

constexpr uint64_t GTT_PAGE_SIZE = 4096;

// PCI config registers of the IGD.
constexpr uint16_t PCI_GGC = 0x50;    // GMCH graphics control (16-bit)
constexpr uint16_t PCI_BDSM = 0x5C;   // base of data stolen memory

// GGC (PCI config 0x50, "GMCH control"), Gen8+ encoding: GGMS (bits 7:6) is
// the GTT's size in stolen memory, GMS (bits 15:8) the graphics data stolen
// memory. i915: gen8_get_total_gtt_size, gen9_get_stolen_size.
struct GgcInfo {
    uint64_t gsmBytes;      // the GTT itself (8 bytes per entry)
    uint64_t stolenBytes;   // graphics data stolen memory (at BDSM)
    uint64_t ggttBytes;     // the address space the GTT maps
};

inline GgcInfo decodeGgc(uint16_t ggc) {
    GgcInfo info = {};
    const uint32_t ggms = (ggc >> 6) & 0x3;
    info.gsmBytes = ggms ? (1ULL << ggms) << 20 : 0;
    const uint32_t gms = (ggc >> 8) & 0xFF;
    info.stolenBytes = gms < 0xF0 ? gms * (32ULL << 20) : (gms - 0xF0 + 1) * (4ULL << 20);
    info.ggttBytes = info.gsmBytes / 8 * GTT_PAGE_SIZE;
    return info;
}

// BDSM (PCI config 0x5C): base of data stolen memory, 1 MiB aligned; bit 0 is
// the lock bit.
inline uint64_t decodeBdsm(uint32_t bdsm) {
    return bdsm & 0xFFF00000u;
}

// On Gen8+ the GTT sits in the upper half of GTTMMADR (BAR0, 16 MiB on Gen9),
// the registers in the lower half: i915's gen6_gttadr_offset.
constexpr uint64_t GTTMMADR_BYTES = 16ULL << 20;
constexpr uint64_t GSM_OFFSET_IN_BAR0 = GTTMMADR_BYTES / 2;

// A Gen8 GGTT entry: the page's physical address and bit 0, present. The
// caching of GGTT accesses comes from PPAT entry 0 (gen8_ggtt_pte_encode).
constexpr uint64_t GEN8_PTE_PRESENT = 1;
constexpr uint64_t GEN8_PTE_ADDR_MASK = 0x0000007FFFFFF000ULL;   // 39-bit physical

inline uint64_t encodePte(uint64_t phys) {
    return (phys & GEN8_PTE_ADDR_MASK) | GEN8_PTE_PRESENT;
}

inline bool pteValid(uint64_t pte) {
    return (pte & GEN8_PTE_PRESENT) != 0;
}

inline uint64_t ptePhys(uint64_t pte) {
    return pte & GEN8_PTE_ADDR_MASK;
}

// Engines and execlists ---------------------------------------------------------
// From i915 (intel_engine_regs.h, intel_gt_regs.h, intel_gpu_commands.h,
// intel_lrc.c, intel_lrc_reg.h; MIT).

constexpr uint32_t RENDER_RING_BASE = 0x02000;
constexpr uint32_t BLT_RING_BASE = 0x22000;

// Registers relative to an engine's base.
constexpr uint32_t RING_TAIL = 0x30;
constexpr uint32_t RING_HEAD = 0x34;
constexpr uint32_t RING_START = 0x38;
constexpr uint32_t RING_CTL = 0x3c;
constexpr uint32_t RING_IPEHR = 0x68;      // the header of the instruction being executed
constexpr uint32_t RING_INSTDONE = 0x6c;
constexpr uint32_t RING_ACTHD = 0x74;
constexpr uint32_t RING_BBADDR = 0x140;     // the batch the engine is in
constexpr uint32_t RING_HWS_PGA = 0x80;
constexpr uint32_t RING_HWSTAM = 0x98;
constexpr uint32_t RING_MI_MODE = 0x9c;
constexpr uint32_t RING_IMR = 0xa8;
constexpr uint32_t RING_EIR = 0xb0;
constexpr uint32_t RING_ESR = 0xb8;
constexpr uint32_t RING_RESET_CTL = 0xd0;
constexpr uint32_t RING_ELSP = 0x230;
constexpr uint32_t RING_EXECLIST_STATUS_LO = 0x234;
constexpr uint32_t RING_EXECLIST_STATUS_HI = 0x238;
constexpr uint32_t RING_MODE_GEN7 = 0x29c;
constexpr uint32_t RING_EXECLISTS_STATUS_BUF = 0x370;   // 6 entries of 8 bytes
constexpr uint32_t RING_CONTEXT_STATUS_PTR = 0x3a0;
constexpr uint32_t CSB_ENTRIES = 6;

constexpr uint32_t STOP_RING = 1u << 8;          // RING_MI_MODE
constexpr uint32_t MODE_IDLE = 1u << 9;
constexpr uint32_t GFX_RUN_LIST_ENABLE = 1u << 15;   // RING_MODE_GEN7
constexpr uint32_t RESET_CTL_REQUEST_RESET = 1u << 0;
constexpr uint32_t RESET_CTL_READY_TO_RESET = 1u << 1;
constexpr uint32_t RESET_CTL_CAT_ERROR = 1u << 2;
constexpr uint32_t RING_VALID = 1;

// GT registers.
constexpr uint32_t REG_GDRST = 0x941c;            // GEN6_GDRST
constexpr uint32_t GRDOM_FULL = 1u << 0;
constexpr uint32_t GRDOM_RENDER = 1u << 1;
constexpr uint32_t GRDOM_BLT = 1u << 3;

// Interrupts (Gen8+; intel_gmd_interrupt_regs.h, i915_reg.h). The master
// register gates everything the GPU signals; GT bank 0 holds the render
// (bits 15:0) and blitter (bits 31:16) engines' bits. No forcewake needed.
constexpr uint32_t REG_MASTER_IRQ = 0x44200;
constexpr uint32_t MASTER_IRQ_CONTROL = 1u << 31;
constexpr uint32_t MASTER_GT_RCS_IRQ = 1u << 0;
constexpr uint32_t MASTER_GT_BCS_IRQ = 1u << 1;
constexpr uint32_t REG_GT_IMR0 = 0x44304;
constexpr uint32_t REG_GT_IIR0 = 0x44308;
constexpr uint32_t REG_GT_IER0 = 0x4430c;
constexpr uint32_t REG_PCU_IER = 0x444ec;
constexpr uint32_t RCS_IRQ_SHIFT = 0;
constexpr uint32_t BCS_IRQ_SHIFT = 16;
constexpr uint32_t GT_RENDER_USER_INTERRUPT = 1u << 0;
constexpr uint32_t GT_CS_MASTER_ERROR_INTERRUPT = 1u << 3;
constexpr uint32_t GT_CONTEXT_SWITCH_INTERRUPT = 1u << 8;

// Forcewake (Gen9): a request register and its ack per power domain.
constexpr uint32_t FORCEWAKE_RENDER_GEN9 = 0xa278;
constexpr uint32_t FORCEWAKE_ACK_RENDER_GEN9 = 0x0d84;
constexpr uint32_t FORCEWAKE_GT_GEN9 = 0xa188;
constexpr uint32_t FORCEWAKE_ACK_GT_GEN9 = 0x130044;
constexpr uint32_t FORCEWAKE_MEDIA_GEN9 = 0xa270;
constexpr uint32_t FORCEWAKE_ACK_MEDIA_GEN9 = 0x0d88;
constexpr uint32_t FORCEWAKE_KERNEL = 1u << 0;

// Masked registers: the upper half says which bits of the lower half to write.
constexpr uint32_t maskedEnable(uint32_t bits) { return bits << 16 | bits; }
constexpr uint32_t maskedDisable(uint32_t bits) { return bits << 16; }
constexpr uint32_t maskedField(uint32_t mask, uint32_t value) { return mask << 16 | value; }

// MI commands (the MI client is 0).
constexpr uint32_t miInstr(uint32_t opcode, uint32_t flags) { return opcode << 23 | flags; }
constexpr uint32_t MI_NOOP = 0;
constexpr uint32_t MI_BATCH_BUFFER_END = miInstr(0x0a, 0);
constexpr uint32_t MI_ARB_CHECK = miInstr(0x05, 0);
constexpr uint32_t MI_STORE_DWORD_IMM_GEN4 = miInstr(0x20, 2);   // + address lo, hi, value
constexpr uint32_t MI_USE_GGTT = 1u << 22;
constexpr uint32_t MI_FLUSH_DW = miInstr(0x26, 1);   // +1 length on Gen8+: address lo, hi, value
constexpr uint32_t MI_FLUSH_DW_OP_STOREDW = 1u << 14;
constexpr uint32_t MI_FLUSH_DW_USE_GTT = 1u << 2;
constexpr uint32_t MI_USER_INTERRUPT = miInstr(0x02, 0);
constexpr uint32_t MI_SEMAPHORE_WAIT = miInstr(0x1c, 2);         // + value, address lo, hi
constexpr uint32_t MI_SEMAPHORE_GLOBAL_GTT = 1u << 22;
constexpr uint32_t MI_SEMAPHORE_POLL = 1u << 15;
constexpr uint32_t MI_SEMAPHORE_SAD_NEQ_SDD = 5u << 12;
constexpr uint32_t miLoadRegisterImm(uint32_t count) { return miInstr(0x22, 2 * count - 1); }
constexpr uint32_t MI_LRI_FORCE_POSTED = 1u << 12;

// A logical ring context: a per-process status page, then the register
// state, which the engine reads as a batch of MI_LOAD_REGISTER_IMMs.
constexpr uint64_t LRC_STATE_OFFSET = GTT_PAGE_SIZE;
constexpr uint32_t LRC_STATE_DWORDS = GTT_PAGE_SIZE / 4;

// Dword indexes of values in the register state (intel_lrc_reg.h).
constexpr uint32_t CTX_CONTEXT_CONTROL = 0x02 + 1;
constexpr uint32_t CTX_RING_HEAD = 0x04 + 1;
constexpr uint32_t CTX_RING_TAIL = 0x06 + 1;
constexpr uint32_t CTX_RING_START = 0x08 + 1;
constexpr uint32_t CTX_RING_CTL = 0x0a + 1;
constexpr uint32_t CTX_TIMESTAMP = 0x22 + 1;
constexpr uint32_t CTX_PDP3_UDW = 0x24 + 1;
constexpr uint32_t CTX_PDP0_LDW = 0x32 + 1;
constexpr uint32_t GEN9_CTX_RING_MI_MODE = 0x54;
constexpr uint32_t CTX_R_PWR_CLK_STATE = 0x42 + 1;   // render contexts only

constexpr uint32_t CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT = 1u << 0;
constexpr uint32_t CTX_CTRL_RS_CTX_ENABLE = 1u << 1;
constexpr uint32_t CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT = 1u << 2;
constexpr uint32_t CTX_CTRL_INHIBIT_SYN_CTX_SWITCH = 1u << 3;

// gen9_xcs_offsets from intel_lrc.c: the layout of a non-render engine's
// register state. A byte with bit 7 is a skip of that many dwords; otherwise
// it starts an LRI (count in bits 5:0, bit 6 posted) followed by register
// offsets from the engine base (7 bits a byte, bit 7 meaning more follow).
#define LRC_NOP(x) (0x80 | (x))
#define LRC_LRI(count, flags) ((flags) << 6 | (count))
#define LRC_POSTED 1
#define LRC_REG(x) ((x) >> 2)
#define LRC_REG16(x) (((x) >> 9) | 0x80), (((x) >> 2) & 0x7f)
inline constexpr uint8_t kGen9XcsOffsets[] = {
    LRC_NOP(1),
    LRC_LRI(14, LRC_POSTED),
    LRC_REG16(0x244), LRC_REG(0x034), LRC_REG(0x030), LRC_REG(0x038), LRC_REG(0x03c),
    LRC_REG(0x168), LRC_REG(0x140), LRC_REG(0x110), LRC_REG(0x11c), LRC_REG(0x114),
    LRC_REG(0x118), LRC_REG(0x1c0), LRC_REG(0x1c4), LRC_REG(0x1c8),

    LRC_NOP(3),
    LRC_LRI(9, LRC_POSTED),
    LRC_REG16(0x3a8), LRC_REG16(0x28c), LRC_REG16(0x288), LRC_REG16(0x284), LRC_REG16(0x280),
    LRC_REG16(0x27c), LRC_REG16(0x278), LRC_REG16(0x274), LRC_REG16(0x270),

    LRC_NOP(13),
    LRC_LRI(1, LRC_POSTED),
    LRC_REG16(0x200),

    LRC_NOP(13),
    LRC_LRI(44, LRC_POSTED),
    LRC_REG(0x028), LRC_REG(0x09c), LRC_REG(0x0c0), LRC_REG(0x178), LRC_REG(0x17c),
    LRC_REG16(0x358), LRC_REG(0x170), LRC_REG(0x150), LRC_REG(0x154), LRC_REG(0x158),
    LRC_REG16(0x41c), LRC_REG16(0x600), LRC_REG16(0x604), LRC_REG16(0x608), LRC_REG16(0x60c),
    LRC_REG16(0x610), LRC_REG16(0x614), LRC_REG16(0x618), LRC_REG16(0x61c), LRC_REG16(0x620),
    LRC_REG16(0x624), LRC_REG16(0x628), LRC_REG16(0x62c), LRC_REG16(0x630), LRC_REG16(0x634),
    LRC_REG16(0x638), LRC_REG16(0x63c), LRC_REG16(0x640), LRC_REG16(0x644), LRC_REG16(0x648),
    LRC_REG16(0x64c), LRC_REG16(0x650), LRC_REG16(0x654), LRC_REG16(0x658), LRC_REG16(0x65c),
    LRC_REG16(0x660), LRC_REG16(0x664), LRC_REG16(0x668), LRC_REG16(0x66c), LRC_REG16(0x670),
    LRC_REG16(0x674), LRC_REG16(0x678), LRC_REG16(0x67c), LRC_REG(0x068),

    0,
};

// gen9_rcs_offsets: the render engine's, the same but for the third block,
// which loads R_PWR_CLK_STATE (0xc8) with a plain (not posted) LRI.
inline constexpr uint8_t kGen9RcsOffsets[] = {
    LRC_NOP(1),
    LRC_LRI(14, LRC_POSTED),
    LRC_REG16(0x244), LRC_REG(0x034), LRC_REG(0x030), LRC_REG(0x038), LRC_REG(0x03c),
    LRC_REG(0x168), LRC_REG(0x140), LRC_REG(0x110), LRC_REG(0x11c), LRC_REG(0x114),
    LRC_REG(0x118), LRC_REG(0x1c0), LRC_REG(0x1c4), LRC_REG(0x1c8),

    LRC_NOP(3),
    LRC_LRI(9, LRC_POSTED),
    LRC_REG16(0x3a8), LRC_REG16(0x28c), LRC_REG16(0x288), LRC_REG16(0x284), LRC_REG16(0x280),
    LRC_REG16(0x27c), LRC_REG16(0x278), LRC_REG16(0x274), LRC_REG16(0x270),

    LRC_NOP(13),
    LRC_LRI(1, 0),
    LRC_REG(0x0c8),

    LRC_NOP(13),
    LRC_LRI(44, LRC_POSTED),
    LRC_REG(0x028), LRC_REG(0x09c), LRC_REG(0x0c0), LRC_REG(0x178), LRC_REG(0x17c),
    LRC_REG16(0x358), LRC_REG(0x170), LRC_REG(0x150), LRC_REG(0x154), LRC_REG(0x158),
    LRC_REG16(0x41c), LRC_REG16(0x600), LRC_REG16(0x604), LRC_REG16(0x608), LRC_REG16(0x60c),
    LRC_REG16(0x610), LRC_REG16(0x614), LRC_REG16(0x618), LRC_REG16(0x61c), LRC_REG16(0x620),
    LRC_REG16(0x624), LRC_REG16(0x628), LRC_REG16(0x62c), LRC_REG16(0x630), LRC_REG16(0x634),
    LRC_REG16(0x638), LRC_REG16(0x63c), LRC_REG16(0x640), LRC_REG16(0x644), LRC_REG16(0x648),
    LRC_REG16(0x64c), LRC_REG16(0x650), LRC_REG16(0x654), LRC_REG16(0x658), LRC_REG16(0x65c),
    LRC_REG16(0x660), LRC_REG16(0x664), LRC_REG16(0x668), LRC_REG16(0x66c), LRC_REG16(0x670),
    LRC_REG16(0x674), LRC_REG16(0x678), LRC_REG16(0x67c), LRC_REG(0x068),

    0,
};
#undef LRC_NOP
#undef LRC_LRI
#undef LRC_POSTED
#undef LRC_REG
#undef LRC_REG16

// i915's set_offsets(): writes the LRI headers and register addresses of a
// layout into `regs` (values left as they are) and, with `close`, ends it
// with MI_BATCH_BUFFER_END. Returns where it ended.
inline uint32_t* setLrcOffsets(uint32_t* regs, const uint8_t* data, uint32_t base, bool close) {
    while (*data) {
        if (*data & 0x80) {
            regs += *data++ & 0x7f;
            continue;
        }
        uint32_t count = *data & 0x3f;
        const uint32_t flags = *data >> 6;
        ++data;
        *regs = miLoadRegisterImm(count);
        if (flags & 1) {
            *regs |= MI_LRI_FORCE_POSTED;
        }
        ++regs;
        do {
            uint32_t offset = 0;
            uint8_t v;
            do {
                v = *data++;
                offset <<= 7;
                offset |= v & 0x7f;
            } while (v & 0x80);
            regs[0] = base + (offset << 2);
            regs += 2;
        } while (--count);
    }
    if (close) {
        *regs = MI_BATCH_BUFFER_END;
    }
    return regs;
}

struct ContextSetup {
    const uint8_t* layout;  // kGen9RcsOffsets or kGen9XcsOffsets
    uint32_t engineBase;
    uint32_t ringGgtt;      // the ring buffer's GGTT offset (page aligned)
    uint32_t ringBytes;     // a whole number of pages
    uint32_t head;          // byte offsets into the ring
    uint32_t tail;
    uint64_t pdp[4];        // page directory pointers (legacy 32-bit PPGTT)
    bool fourLevel;         // a 48-bit PPGTT: pml4 instead of pdp[]
    uint64_t pml4;          // its top-level table (physical)
    uint32_t rpcs;          // render only: R_PWR_CLK_STATE (gen9Rpcs), 0 = hardware default
};

// The register state (LRC_STATE_DWORDS dwords) of a fresh context on a Gen9
// engine, as i915's __lrc_init_regs + lrc_update_regs make it with `inhibit`:
// the engine loads only the ring part and saves the rest itself. On the
// render engine R_PWR_CLK_STATE stays 0, the hardware's own slice and
// subslice configuration (i915 computes one from the fuses).
//
// With `fromDefault` the registers already hold a saved image (the engine's
// default context, copied): their values stay, the layout is written again,
// and restore is not inhibited, as __lrc_init_regs does for a context made
// from the default state.
inline void buildContextState(uint32_t* regs, const ContextSetup& s, bool fromDefault = false) {
    if (!fromDefault) {
        for (uint32_t i = 0; i < LRC_STATE_DWORDS; ++i) {
            regs[i] = 0;
        }
    }
    // i915's set_offsets(regs, ..., inhibit): only a restore-inhibited image
    // is closed with MI_BATCH_BUFFER_END. An image copied from the default is
    // a command stream the engine replays on restore, and the render state
    // follows the register list: a BB_END there ends the restore before it,
    // so STATE_BASE_ADDRESS and the rest of the 3D state never came back.
    setLrcOffsets(regs, s.layout, s.engineBase, !fromDefault);

    uint32_t ctl = maskedEnable(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH);
    ctl |= maskedDisable(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT);
    if (!fromDefault) {
        ctl |= CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT;
    }
    ctl |= maskedDisable(CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT | CTX_CTRL_RS_CTX_ENABLE);
    regs[CTX_CONTEXT_CONTROL] = ctl;
    regs[CTX_TIMESTAMP] = 0;

    // i915's init_ppgtt_regs: a 4-level PPGTT puts its PML4 in PDP0 (the
    // other PDPs are ignored); a 3-level one fills all four.
    for (int n = 0; n < 4; ++n) {
        // PDP3 first: UDW at CTX_PDP3_UDW, then LDW, down to PDP0.
        const uint32_t udw = CTX_PDP3_UDW + static_cast<uint32_t>(3 - n) * 4;
        const uint64_t value = s.fourLevel ? (n == 0 ? s.pml4 : 0) : s.pdp[n];
        regs[udw] = static_cast<uint32_t>(value >> 32);
        regs[udw + 2] = static_cast<uint32_t>(value);
    }

    regs[GEN9_CTX_RING_MI_MODE + 1] &= ~STOP_RING;
    regs[GEN9_CTX_RING_MI_MODE + 1] |= STOP_RING << 16;

    regs[CTX_RING_START] = s.ringGgtt;
    regs[CTX_RING_HEAD] = s.head;
    regs[CTX_RING_TAIL] = s.tail;
    regs[CTX_RING_CTL] = (s.ringBytes - static_cast<uint32_t>(GTT_PAGE_SIZE)) | RING_VALID;
    if (s.rpcs != 0) {
        regs[CTX_R_PWR_CLK_STATE] = s.rpcs;
    }
}

// The execlist descriptor of a context at GGTT `lrca` (its first page): a
// privileged legacy 32-bit context (i915's lrc_descriptor) with a software
// context id in the upper half.
constexpr uint64_t GEN8_CTX_VALID = 1u << 0;
constexpr uint64_t GEN8_CTX_PRIVILEGE = 1u << 8;
constexpr uint64_t GEN8_CTX_ADDRESSING_MODE_SHIFT = 3;
constexpr uint64_t INTEL_LEGACY_32B_CONTEXT = 1;
constexpr uint64_t INTEL_LEGACY_64B_CONTEXT = 3;   // a 48-bit (4-level) PPGTT

inline uint64_t contextDescriptor(uint32_t lrca, uint32_t contextId, bool fourLevel = false) {
    uint64_t desc = (fourLevel ? INTEL_LEGACY_64B_CONTEXT : INTEL_LEGACY_32B_CONTEXT)
                    << GEN8_CTX_ADDRESSING_MODE_SHIFT;
    desc |= GEN8_CTX_VALID | GEN8_CTX_PRIVILEGE;
    desc |= lrca & 0xFFFFF000u;
    desc |= static_cast<uint64_t>(contextId) << 32;
    return desc;
}

// The private PAT (i915's bdw_setup_private_ppat, Gen9): the eight memory
// types a GPU page-table entry selects with its PWT, PCD and PAT bits, as on
// the CPU. Index 0 (an entry with none of them, so also every GGTT entry) is
// write-back in the LLC; 4-7 write-back in LLC and eLLC with LRU ages; 3 is
// uncached. Write-back in the LLC is what keeps the GPU coherent with the
// CPU's caches on a part with an LLC, so neither side needs to flush.
constexpr uint32_t GEN8_PRIVATE_PAT_LO = 0x40e0;
constexpr uint32_t GEN8_PRIVATE_PAT_HI = 0x40e4;
constexpr uint64_t GEN8_PPAT_UC = 0;
constexpr uint64_t GEN8_PPAT_WC = 1;
constexpr uint64_t GEN8_PPAT_WT = 2;
constexpr uint64_t GEN8_PPAT_WB = 3;
constexpr uint64_t GEN8_PPAT_ELLC_OVERRIDE = 0u << 2;
constexpr uint64_t GEN8_PPAT_LLC = 1u << 2;
constexpr uint64_t GEN8_PPAT_LLCELLC = 2u << 2;
constexpr uint64_t gen8PpatAge(uint64_t age) { return age << 4; }
constexpr uint64_t gen8Ppat(uint32_t index, uint64_t value) { return value << (index * 8); }
constexpr uint64_t kGen9PrivatePat =
    gen8Ppat(0, GEN8_PPAT_WB | GEN8_PPAT_LLC) |
    gen8Ppat(1, GEN8_PPAT_WC | GEN8_PPAT_LLCELLC) |
    gen8Ppat(2, GEN8_PPAT_WB | GEN8_PPAT_ELLC_OVERRIDE) |   // scanout with eLLC (Gen9)
    gen8Ppat(3, GEN8_PPAT_UC) |
    gen8Ppat(4, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | gen8PpatAge(0)) |
    gen8Ppat(5, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | gen8PpatAge(1)) |
    gen8Ppat(6, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | gen8PpatAge(2)) |
    gen8Ppat(7, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | gen8PpatAge(3));

// A 48-bit PPGTT (i915's gen8_ppgtt): four levels of 512 eight-byte entries,
// PML4 -> PDP -> PD -> PT -> 4 KiB page, every table one page. An entry is
// the next level's physical address, present and writable, plus the PPAT
// index bits (x86's PWT bit 3, PCD bit 4, PAT bit 7). gen8_pte_encode:
// cached pages are PPAT index 4 (PAT), uncached index 3 (PWT|PCD); tables
// (gen8_pde_encode) index 0.
constexpr uint32_t PPGTT_VA_BITS = 48;
constexpr uint64_t PPGTT_VA_LIMIT = 1ULL << PPGTT_VA_BITS;
constexpr uint32_t PPGTT_ENTRIES = 512;
constexpr uint64_t GEN8_PAGE_PRESENT = 1u << 0;
constexpr uint64_t GEN8_PAGE_RW = 1u << 1;
constexpr uint64_t PPAT_UNCACHED = (1u << 3) | (1u << 4);
constexpr uint64_t PPAT_CACHED = 1u << 7;
constexpr uint64_t PPAT_CACHED_PDE = 0;
constexpr uint64_t PPGTT_ADDRESS_MASK = 0x0000FFFFFFFFF000ULL;

constexpr uint64_t ppgttPte(uint64_t phys, bool cached) {
    return (phys & PPGTT_ADDRESS_MASK) | GEN8_PAGE_PRESENT | GEN8_PAGE_RW | (cached ? PPAT_CACHED : PPAT_UNCACHED);
}
constexpr uint64_t ppgttPde(uint64_t tablePhys) {
    return (tablePhys & PPGTT_ADDRESS_MASK) | GEN8_PAGE_PRESENT | GEN8_PAGE_RW | PPAT_CACHED_PDE;
}
constexpr uint64_t ppgttEntryAddress(uint64_t entry) { return entry & PPGTT_ADDRESS_MASK; }
// The entry index of `va` in a table of `level` (3 = PML4 ... 0 = PT).
constexpr uint32_t ppgttIndex(uint64_t va, uint32_t level) {
    return static_cast<uint32_t>((va >> (12 + 9 * level)) & (PPGTT_ENTRIES - 1));
}

// MI_BATCH_BUFFER_START on Gen8+: two address dwords; bit 8 picks the
// context's PPGTT (a non-privileged batch) over the GGTT.
constexpr uint32_t MI_BATCH_BUFFER_START_GEN8 = miInstr(0x31, 1);
constexpr uint32_t MI_BATCH_PPGTT = 1u << 8;

// GGTT pages, one bit each in storage the caller provides (wordsFor(pages)
// words; 128 KiB for a 4 GiB GGTT): first-fit allocation with alignment and an
// upper limit (the CPU-mappable aperture), reservations for what the firmware
// uses. A bitmap rather than a list of free ranges, so that freeing and
// reserving never fail however fragmented the space gets.
class GttPageAllocator {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    static constexpr uint64_t wordsFor(uint32_t pages) { return (static_cast<uint64_t>(pages) + 63) / 64; }

    // All pages free. With no storage (or 0 pages) nothing can be allocated.
    void init(uint64_t* storage, uint32_t totalPages) {
        words = storage;
        total = storage ? totalPages : 0;
        freeCount = total;
        for (uint64_t i = 0; i < wordsFor(total); ++i) {
            words[i] = 0;
        }
    }

    // Takes [start, start+pages) out of the free space (the part that is in
    // range and not already taken).
    void reserve(uint32_t start, uint32_t pages) {
        uint64_t end = static_cast<uint64_t>(start) + pages;
        if (end > total) {
            end = total;
        }
        for (uint64_t page = start; page < end; ++page) {
            if (!used(page)) {
                set(page, true);
                --freeCount;
            }
        }
    }

    // The first start (a multiple of alignPages, ending at or below
    // limitPages when that is not 0) with pages free pages, now taken; kNone
    // if there is none.
    uint32_t alloc(uint32_t pages, uint32_t alignPages = 1, uint32_t limitPages = 0) {
        if (pages == 0 || pages > freeCount) {
            return kNone;
        }
        if (alignPages == 0) {
            alignPages = 1;
        }
        const uint64_t limit = (limitPages && limitPages < total) ? limitPages : total;
        uint64_t start = 0;
        while (true) {
            start = (start + alignPages - 1) / alignPages * alignPages;
            if (start + pages > limit) {
                return kNone;
            }
            const uint64_t taken = firstUsed(start, start + pages);
            if (taken == kNoBit) {
                break;
            }
            start = taken + 1;
        }
        for (uint64_t page = start; page < start + pages; ++page) {
            set(page, true);
        }
        freeCount -= pages;
        return static_cast<uint32_t>(start);
    }

    // Gives [start, start+pages) back. False, changing nothing, if any of it
    // is out of range or already free (a double free).
    bool free(uint32_t start, uint32_t pages) {
        const uint64_t end = static_cast<uint64_t>(start) + pages;
        if (end > total) {
            return false;
        }
        for (uint64_t page = start; page < end; ++page) {
            if (!used(page)) {
                return false;
            }
        }
        for (uint64_t page = start; page < end; ++page) {
            set(page, false);
        }
        freeCount += pages;
        return true;
    }

    uint64_t freePages() const { return freeCount; }
    uint32_t totalPages() const { return total; }
    bool isUsed(uint32_t page) const { return page < total && used(page); }

private:
    static constexpr uint64_t kNoBit = ~0ULL;

    bool used(uint64_t page) const { return (words[page / 64] >> (page % 64)) & 1; }

    void set(uint64_t page, bool value) {
        const uint64_t bit = 1ULL << (page % 64);
        if (value) {
            words[page / 64] |= bit;
        } else {
            words[page / 64] &= ~bit;
        }
    }

    // The first taken page in [from, to), or kNoBit; whole words at a time.
    uint64_t firstUsed(uint64_t from, uint64_t to) const {
        uint64_t page = from;
        while (page < to) {
            const uint64_t shift = page % 64;
            const uint64_t bits = words[page / 64] >> shift;
            if (bits != 0) {
                const uint64_t found = page + static_cast<uint64_t>(__builtin_ctzll(bits));
                return found < to ? found : kNoBit;
            }
            page += 64 - shift;
        }
        return kNoBit;
    }

    uint64_t* words = nullptr;
    uint32_t total = 0;
    uint64_t freeCount = 0;
};
} // namespace intel_gen9
