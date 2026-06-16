#pragma once

#include <stddef.h>
#include <stdint.h>
#include <graphics/framebuffer.hpp>

// Intel HD Graphics 530 / Skylake (Gen9) integrated GPU support.
//
// This driver targets the Intel Gen9 (Skylake) integrated display engine. The
// flagship target is "HD Graphics 530" (Skylake GT2, PCI device 0x1912 desktop
// and 0x191B mobile/halo), but the whole Skylake GT1..GT4 family is recognised.
//
// Scope and honesty: real Intel HD 530 silicon is not emulated by QEMU, so the
// runtime-verified path here is conservative and bring-up oriented:
//
//   * PCI detection of the Gen9 family by vendor/device id (verified by tests).
//   * Safe decode + identity mapping of the GTTMMADR (BAR0, MMIO+GTT) and GMADR
//     (BAR2, aperture) base address registers, with size and bounds validation.
//   * Read-back of the firmware/GOP-initialised display plane registers
//     (PLANE_CTL / PLANE_STRIDE / PLANE_SURF / PIPE_SRCSZ) to recover the active
//     scanout (the "inherit firmware mode" strategy used by early i915 bring-up).
//   * Presenting that linear scanout surface as the kernel framebuffer.
//
// Everything that cannot be verified in this environment (full atomic modeset,
// DPLL/CDCLK reprogramming, GTT remapping of new buffers, the RCS command ring,
// and render acceleration) is intentionally NOT enabled and reports as an
// unsupported capability rather than faking success. See docs/intel-hd530.md.

namespace intel_gen9 {

// PCI identity ---------------------------------------------------------------
constexpr uint16_t INTEL_VENDOR_ID = 0x8086;

// PCI class for a VGA-compatible display controller (class 0x03, subclass 0x00).
constexpr uint8_t PCI_CLASS_DISPLAY = 0x03;
constexpr uint8_t PCI_SUBCLASS_VGA = 0x00;

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

// Returns a pointer to the matching Skylake/Gen9 device descriptor, or nullptr
// if the (vendor, device) pair is not a supported Gen9 part. Only Intel vendor
// id 0x8086 matches. This is the single source of truth for "do we drive it".
const Gen9DeviceInfo* lookupDevice(uint16_t vendorId, uint16_t deviceId);

// True when the device id is one of the HD Graphics 530 SKUs (0x1912/0x191B).
bool isHd530(uint16_t deviceId);

// MMIO / BAR layout ----------------------------------------------------------
//
// On Gen9 the PCI BARs of the IGD (function 0:2.0) are:
//   BAR0 (0x10): GTTMMADR - MMIO register window; the upper half is the GTT.
//                64-bit, prefetchable=0. 16 MiB on Gen9 (2 MiB regs + GTT).
//   BAR2 (0x18): GMADR    - the graphics aperture (mappable through the GTT).
//                64-bit, prefetchable. Size encoded by the device.
//   BAR4 (0x20): I/O BAR  - legacy index/data I/O port pair.
constexpr uint16_t PCI_BAR0_OFFSET = 0x10; // GTTMMADR low dword.
constexpr uint16_t PCI_BAR2_OFFSET = 0x18; // GMADR low dword.
constexpr uint16_t PCI_BAR4_OFFSET = 0x20; // I/O BAR.

// The MMIO register block is the low 2 MiB of GTTMMADR; the GTT occupies the
// remaining space above it. Gen9 GTTMMADR is 16 MiB total.
constexpr uint64_t GTTMMADR_SIZE = 16ull * 1024 * 1024;
constexpr uint64_t MMIO_REG_SIZE = 2ull * 1024 * 1024;
constexpr uint64_t GTT_OFFSET = MMIO_REG_SIZE; // GTT starts after the regs.

// Display engine registers (Gen9 / Skylake). Offsets are relative to the MMIO
// register window base (GTTMMADR + 0). These are stable, documented values
// from the Intel Skylake PRM (vol 2c, display registers).
constexpr uint32_t REG_PIPE_A_BASE = 0x60000;

// PIPE_SRCSZ_A: horizontal/vertical source size of pipe A's active region.
//   bits [28:16] = width-1, bits [12:0] = height-1.
constexpr uint32_t REG_PIPE_SRCSZ_A = 0x6001C;

// Universal plane 1 (primary) on pipe A.
constexpr uint32_t REG_PLANE_CTL_A = 0x70180;     // enable + pixel format.
constexpr uint32_t REG_PLANE_STRIDE_A = 0x70188;  // stride in 64-byte tiles/units.
constexpr uint32_t REG_PLANE_SIZE_A = 0x70190;    // (height-1)<<16 | (width-1).
constexpr uint32_t REG_PLANE_SURF_A = 0x7019C;    // surface base addr (in GMADR/GTT).

// PLANE_CTL bit fields used to interpret the firmware-programmed plane.
constexpr uint32_t PLANE_CTL_ENABLE = 1u << 31;
constexpr uint32_t PLANE_CTL_FORMAT_MASK = 0xfu << 24;
constexpr uint32_t PLANE_CTL_FORMAT_XRGB8888 = 0x4u << 24; // 8:8:8:8 (BGRX/XRGB).
constexpr uint32_t PLANE_CTL_TILED_MASK = 0x7u << 10;
constexpr uint32_t PLANE_CTL_TILED_LINEAR = 0x0u << 10;

// PLANE_STRIDE on Gen9 is expressed in units of 64 bytes.
constexpr uint32_t PLANE_STRIDE_UNIT = 64;

// Recovered display configuration -------------------------------------------
struct DisplayMode {
    uint32_t width;       // active pixels per scanline.
    uint32_t height;      // active scanlines.
    uint32_t stride;      // bytes per scanline.
    uint32_t bpp;         // bits per pixel (32 for XRGB8888).
    uint64_t surfaceGtt;  // PLANE_SURF value: offset into GMADR/GTT space.
    bool tiledLinear;     // true if the firmware left the plane in linear tiling.
    bool valid;           // true if a usable, enabled plane was recovered.
};

class IntelGen9Driver {
public:
    static IntelGen9Driver& get();

    // Probe the PCI bus for a supported Gen9 part. Cheap; no MMIO touched.
    bool probe();

    // Full bring-up: enable PCI memory decode + bus mastering, map the BARs,
    // recover the firmware display mode, and expose a linear framebuffer.
    // The supplied framebuffer provides the safe fallback geometry/base that is
    // used if the firmware plane cannot be interpreted. Returns false (without
    // crashing or corrupting state) on any unsupported / invalid condition.
    bool initialize(iFramebuffer& fallback);

    bool isInitialized() const { return initialized; }
    bool isPresent() const { return deviceFound; }

    const Gen9DeviceInfo* deviceInfo() const { return info; }
    const DisplayMode& mode() const { return display; }

    // The active scanout framebuffer (CPU-visible pointer). On the conservative
    // path this is the firmware/GOP framebuffer the bootloader handed us.
    void* getFramebuffer() const { return framebuffer; }
    uint32_t getWidth() const { return display.width; }
    uint32_t getHeight() const { return display.height; }
    uint32_t getPitch() const { return display.stride; }
    uint32_t getFramebufferSize() const { return fbSize; }

    // Present the CPU-drawn framebuffer. On the inherit-firmware path scanout is
    // already coherent (CPU writes land directly in the scanned-out surface), so
    // this is a no-op success. A real modeset path would flush/transfer here.
    bool flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

    // PCI bus/device/function where the part was found (valid once probed).
    uint8_t pciBus() const { return bus; }
    uint8_t pciDevice() const { return device; }
    uint8_t pciFunction() const { return function; }

    uint64_t mmioBase() const { return gttmmadr; }
    uint64_t apertureBase() const { return gmadr; }

    // --- Pure helpers exposed for unit testing (no hardware access) ---------

    // Decode a 64-bit memory BAR pair into a base address. Returns 0 on an I/O
    // BAR, a disabled BAR (0 / all-ones), or an unmapped BAR.
    static uint64_t decodeMemoryBar(uint32_t barLow, uint32_t barHigh);

    // Interpret raw display register values into a DisplayMode. Pure function so
    // it can be exercised without a GPU. fallbackW/H/stride are used when the
    // plane is disabled or its size reads back as zero.
    static DisplayMode interpretPlane(uint32_t planeCtl, uint32_t planeStride,
                                      uint32_t pipeSrcsz, uint32_t planeSurf,
                                      uint32_t fallbackW, uint32_t fallbackH,
                                      uint32_t fallbackStride);

    // Volatile MMIO accessors (relative to the mapped GTTMMADR register window).
    uint32_t readReg(uint32_t offset) const;
    void writeReg(uint32_t offset, uint32_t value);

private:
    IntelGen9Driver() = default;

    bool detectDevice();
    bool mapBars();
    bool recoverDisplayMode(iFramebuffer& fallback);

    uint16_t readConfig16(uint16_t offset) const;
    uint32_t readConfig32(uint16_t offset) const;
    void writeConfig16(uint16_t offset, uint16_t value);

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
};

} // namespace intel_gen9
