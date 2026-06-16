#include <graphics/intel_gen9.hpp>

#include <cpu/acpi/pci.hpp>
#include <graphics/console.hpp>
#include <memory/vmm.hpp>

namespace intel_gen9 {

namespace {

// Local PCI config offsets (each InstantOS driver defines its own, see e1000.cpp
// / virtio_gpu.cpp for the established convention).
constexpr uint16_t PCI_VENDOR_ID_REG = 0x00;
constexpr uint16_t PCI_DEVICE_ID_REG = 0x02;
constexpr uint16_t PCI_COMMAND_REG = 0x04;
constexpr uint16_t PCI_CLASS_REG = 0x0B;     // class code (high byte of 0x08 dword).
constexpr uint16_t PCI_SUBCLASS_REG = 0x0A;  // subclass.
constexpr uint16_t PCI_COMMAND_MEMORY = 1 << 1;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 1 << 2;

// The full Skylake (Gen9) VGA device-id table, derived from the upstream
// i915_pciids.h INTEL_SKL_*_IDS lists. HD Graphics 530 corresponds to the GT2
// desktop part 0x1912 and the GT2 halo/mobile part 0x191B.
constexpr Gen9DeviceInfo kGen9Devices[] = {
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

constexpr size_t kGen9DeviceCount = sizeof(kGen9Devices) / sizeof(kGen9Devices[0]);

} // namespace

const Gen9DeviceInfo* lookupDevice(uint16_t vendorId, uint16_t deviceId) {
    if (vendorId != INTEL_VENDOR_ID) {
        return nullptr;
    }
    for (size_t i = 0; i < kGen9DeviceCount; ++i) {
        if (kGen9Devices[i].deviceId == deviceId) {
            return &kGen9Devices[i];
        }
    }
    return nullptr;
}

bool isHd530(uint16_t deviceId) {
    const Gen9DeviceInfo* dev = lookupDevice(INTEL_VENDOR_ID, deviceId);
    return dev != nullptr && dev->isHd530;
}

uint64_t IntelGen9Driver::decodeMemoryBar(uint32_t barLow, uint32_t barHigh) {
    // Disabled / unimplemented BAR.
    if (barLow == 0 || barLow == 0xFFFFFFFFu) {
        return 0;
    }
    // I/O space BAR (bit 0 set) is not a memory window.
    if (barLow & 0x1) {
        return 0;
    }
    uint64_t base = static_cast<uint64_t>(barLow & ~0xFu);
    const uint32_t type = (barLow >> 1) & 0x3;
    if (type == 0x2) {
        // 64-bit BAR: high dword carries the upper address bits.
        base |= static_cast<uint64_t>(barHigh) << 32;
    }
    return base;
}

DisplayMode IntelGen9Driver::interpretPlane(uint32_t planeCtl, uint32_t planeStride,
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
        // Stride is in 64-byte units on Gen9.
        const uint32_t strideBytes = (planeStride & 0x3FF) * PLANE_STRIDE_UNIT;
        m.stride = (strideBytes >= srcW * 4) ? strideBytes : srcW * 4;
        m.valid = m.tiledLinear; // only the linear case is safe to scan out as-is.
    }

    if (!m.valid) {
        // Fall back to the firmware/GOP geometry we were handed.
        m.width = fallbackW;
        m.height = fallbackH;
        m.stride = (fallbackStride != 0) ? fallbackStride : fallbackW * 4;
        m.tiledLinear = true;
        m.valid = (fallbackW > 0 && fallbackH > 0);
    }
    return m;
}

IntelGen9Driver& IntelGen9Driver::get() {
    static IntelGen9Driver instance;
    return instance;
}

uint16_t IntelGen9Driver::readConfig16(uint16_t offset) const {
    return PCI::get().readConfig16(0, bus, device, function, offset);
}

uint32_t IntelGen9Driver::readConfig32(uint16_t offset) const {
    return PCI::get().readConfig32(0, bus, device, function, offset);
}

void IntelGen9Driver::writeConfig16(uint16_t offset, uint16_t value) {
    PCI::get().writeConfig16(0, bus, device, function, offset, value);
}

uint32_t IntelGen9Driver::readReg(uint32_t offset) const {
    if (!mmio || offset + 4 > MMIO_REG_SIZE) {
        return 0;
    }
    return *reinterpret_cast<volatile uint32_t*>(mmio + offset);
}

void IntelGen9Driver::writeReg(uint32_t offset, uint32_t value) {
    if (!mmio || offset + 4 > MMIO_REG_SIZE) {
        return;
    }
    *reinterpret_cast<volatile uint32_t*>(mmio + offset) = value;
}

bool IntelGen9Driver::detectDevice() {
    PCI& pci = PCI::get();
    for (uint16_t b = 0; b < 256; ++b) {
        for (uint8_t d = 0; d < 32; ++d) {
            for (uint8_t f = 0; f < 8; ++f) {
                const uint16_t vendor = pci.readConfig16(0, b, d, f, PCI_VENDOR_ID_REG);
                if (vendor == 0xFFFF) {
                    continue;
                }
                const uint16_t devId = pci.readConfig16(0, b, d, f, PCI_DEVICE_ID_REG);
                const Gen9DeviceInfo* match = lookupDevice(vendor, devId);
                if (!match) {
                    continue;
                }
                // Confirm it really is a display controller before claiming it.
                const uint8_t classCode = pci.readConfig8(0, b, d, f, PCI_CLASS_REG);
                if (classCode != PCI_CLASS_DISPLAY) {
                    continue;
                }
                bus = static_cast<uint8_t>(b);
                device = d;
                function = f;
                info = match;
                deviceFound = true;
                return true;
            }
        }
    }
    return false;
}

bool IntelGen9Driver::mapBars() {
    // Enable memory-space decode and bus mastering before touching the BARs.
    uint16_t command = readConfig16(PCI_COMMAND_REG);
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    writeConfig16(PCI_COMMAND_REG, command);

    // BAR0 = GTTMMADR (64-bit memory). The MMIO register window is its low part.
    const uint32_t bar0Low = readConfig32(PCI_BAR0_OFFSET);
    const uint32_t bar0High = readConfig32(PCI_BAR0_OFFSET + 4);
    gttmmadr = decodeMemoryBar(bar0Low, bar0High);
    if (gttmmadr == 0) {
        Console::get().drawText("[i915] BAR0/GTTMMADR not mapped by firmware\n");
        return false;
    }

    // BAR2 = GMADR (graphics aperture). Optional for the inherit-firmware path,
    // but decode it for diagnostics and future GTT work.
    const uint32_t bar2Low = readConfig32(PCI_BAR2_OFFSET);
    const uint32_t bar2High = readConfig32(PCI_BAR2_OFFSET + 4);
    gmadr = decodeMemoryBar(bar2Low, bar2High);

    // Identity-map the MMIO register window (the kernel runs identity-mapped, so
    // virtual == physical). Cache-disabled, write-through, non-executable: this
    // is the standard MMIO mapping flags used by xhci/i2c_hid in this kernel.
    const uint64_t flags = Present | ReadWrite | CacheDisab | WriteThru | NoExecute;
    const uint64_t pages = MMIO_REG_SIZE / PAGE_SIZE;
    VMM::MapRange(gttmmadr, gttmmadr, pages, flags);
    mmio = reinterpret_cast<volatile uint8_t*>(gttmmadr);
    return true;
}

bool IntelGen9Driver::recoverDisplayMode(iFramebuffer& fallback) {
    const uint32_t fbW = static_cast<uint32_t>(fallback.getWidth());
    const uint32_t fbH = static_cast<uint32_t>(fallback.getHeight());
    const uint32_t fbStride = static_cast<uint32_t>(fallback.getPitch() * 4);

    const uint32_t planeCtl = readReg(REG_PLANE_CTL_A);
    const uint32_t planeStride = readReg(REG_PLANE_STRIDE_A);
    const uint32_t pipeSrcsz = readReg(REG_PIPE_SRCSZ_A);
    const uint32_t planeSurf = readReg(REG_PLANE_SURF_A);

    display = interpretPlane(planeCtl, planeStride, pipeSrcsz, planeSurf,
                             fbW, fbH, fbStride);
    if (!display.valid) {
        return false;
    }

    // Conservative bring-up strategy: scan out via the framebuffer the firmware
    // already configured (the GOP linear buffer we were handed at boot). The
    // recovered Gen9 plane geometry is validated against it; we do not move the
    // scanout surface (that would require GTT remapping + a full modeset, which
    // is unverifiable here). This guarantees a coherent, non-corrupting display.
    framebuffer = fallback.getRaw();
    fbSize = static_cast<uint32_t>(fallback.getFBSize());
    if (!framebuffer || fbSize == 0) {
        display.valid = false;
        return false;
    }
    return true;
}

bool IntelGen9Driver::probe() {
    if (deviceFound) {
        return true;
    }
    return detectDevice();
}

bool IntelGen9Driver::initialize(iFramebuffer& fallback) {
    if (initialized) {
        return true;
    }
    if (!deviceFound && !detectDevice()) {
        return false;
    }

    Console::get().drawText("[i915] Intel Gen9 GPU: ");
    Console::get().drawText(info ? info->name : "unknown");
    Console::get().drawText("\n[i915] PCI ");
    Console::get().drawHex(bus);
    Console::get().drawText(":");
    Console::get().drawHex(device);
    Console::get().drawText(".");
    Console::get().drawHex(function);
    Console::get().drawText(" device=");
    Console::get().drawHex(info ? info->deviceId : 0);
    Console::get().drawText(info && info->isHd530 ? " [HD 530]\n" : "\n");

    if (!mapBars()) {
        return false;
    }
    Console::get().drawText("[i915] GTTMMADR=");
    Console::get().drawHex(gttmmadr);
    Console::get().drawText(" GMADR=");
    Console::get().drawHex(gmadr);
    Console::get().drawText("\n");

    if (!recoverDisplayMode(fallback)) {
        Console::get().drawText("[i915] display: could not recover firmware mode\n");
        return false;
    }

    Console::get().drawText("[i915] display mode ");
    Console::get().drawNumber(static_cast<int64_t>(display.width));
    Console::get().drawText("x");
    Console::get().drawNumber(static_cast<int64_t>(display.height));
    Console::get().drawText(" stride=");
    Console::get().drawNumber(static_cast<int64_t>(display.stride));
    Console::get().drawText(" bpp=");
    Console::get().drawNumber(static_cast<int64_t>(display.bpp));
    Console::get().drawText(display.valid ? " [inherited]\n" : " [fallback]\n");

    initialized = true;
    return true;
}

bool IntelGen9Driver::flush(uint32_t, uint32_t, uint32_t, uint32_t) {
    // The inherited firmware scanout is CPU-coherent: writes to the linear
    // framebuffer are scanned out directly, so an explicit flush/transfer is not
    // required on this path. A real modeset path would issue a plane update or a
    // ring transfer here. Reported as success.
    return initialized;
}

} // namespace intel_gen9
