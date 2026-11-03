#include <graphics/intel_gen9.hpp>

#include <cpu/acpi/pci.hpp>
#include <graphics/console.hpp>
#include <memory/vmm.hpp>
#include <common/ports.hpp>

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

} // namespace

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

    // The GTT (upper half of BAR0) and the CPU-usable part of the aperture,
    // write-combining as i915 maps them. Mapped now, while there is only the
    // kernel's address space, so every process made later has them; nothing
    // is written to the GPU until initializeGtt().
    const uint64_t wc = Present | ReadWrite | NoExecute | WriteCombine;
    ggc = decodeGgc(readConfig16(PCI_GGC));
    if (ggc.gsmBytes != 0) {
        const uint64_t gsmPhys = gttmmadr + GSM_OFFSET_IN_BAR0;
        VMM::MapRange(gsmPhys, gsmPhys, ggc.gsmBytes / PAGE_SIZE, wc);
        gsm = reinterpret_cast<volatile uint64_t*>(gsmPhys);
        gttEntries = static_cast<uint32_t>(ggc.gsmBytes / sizeof(uint64_t));
    }
    if (gmadr != 0) {
        VMM::MapRange(gmadr, gmadr, APERTURE_USABLE_BYTES / PAGE_SIZE, wc);
    }
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

namespace {

void logValue(const char* name, uint64_t value) {
    Console::get().drawText(" ");
    Console::get().drawText(name);
    Console::get().drawText("=");
    Console::get().drawHex(value);
}

// Register offsets read by logDiagnostics (Skylake PRM / i915_reg.h).
constexpr uint16_t PCI_CAPABILITIES = 0x34;
constexpr uint8_t PCI_CAP_MSI = 0x05;
constexpr uint32_t REG_PIPECONF[3] = { 0x70008, 0x71008, 0x72008 };
constexpr uint32_t REG_GT_FIFO_FREE = 0x120008;
constexpr uint32_t REG_FUSE2 = 0x9120;
// The ring registers are intel_gen9_logic.hpp's.
constexpr uint32_t ENGINE_BASE[2] = { RENDER_RING_BASE, BLT_RING_BASE };
constexpr const char* ENGINE_NAME[2] = { "rcs", "bcs" };
constexpr uint32_t MSR_PAT = 0x277;

} // namespace

void IntelGen9Driver::logDiagnostics() {
    Console::get().drawText("[i915] pci");
    const uint16_t ggc = readConfig16(PCI_GGC);
    logValue("GGC", ggc);
    logValue("BDSM", readConfig32(PCI_BDSM));
    bool msi = false;
    uint8_t cap = static_cast<uint8_t>(readConfig16(PCI_CAPABILITIES) & 0xFC);
    for (int guard = 0; cap != 0 && guard < 32; ++guard) {
        const uint16_t header = readConfig16(cap);
        if ((header & 0xFF) == PCI_CAP_MSI) {
            msi = true;
        }
        cap = static_cast<uint8_t>((header >> 8) & 0xFC);
    }
    Console::get().drawText(msi ? " msi=yes\n" : " msi=no\n");

    Console::get().drawText("[i915] display");
    static const char* const kPipeNames[3] = { "PIPEACONF", "PIPEBCONF", "PIPECCONF" };
    for (int pipe = 0; pipe < 3; ++pipe) {
        logValue(kPipeNames[pipe], readReg(REG_PIPECONF[pipe]));
    }
    logValue("PLANE_CTL_A", readReg(REG_PLANE_CTL_A));
    logValue("PLANE_SURF_A", readReg(REG_PLANE_SURF_A));
    Console::get().drawText("\n[i915] gt");
    logValue("FORCEWAKE_ACK", readReg(FORCEWAKE_ACK_RENDER_GEN9));
    logValue("GT_FIFO_FREE", readReg(REG_GT_FIFO_FREE));
    logValue("FUSE2", readReg(REG_FUSE2));
    Console::get().drawText("\n");
    for (int engine = 0; engine < 2; ++engine) {
        const uint32_t base = ENGINE_BASE[engine];
        Console::get().drawText("[i915] ");
        Console::get().drawText(ENGINE_NAME[engine]);
        logValue("HEAD", readReg(base + RING_HEAD));
        logValue("TAIL", readReg(base + RING_TAIL));
        logValue("START", readReg(base + RING_START));
        logValue("CTL", readReg(base + RING_CTL));
        logValue("MODE", readReg(base + RING_MODE_GEN7));
        logValue("EXECLIST_STATUS", readReg(base + RING_EXECLIST_STATUS_LO));
        Console::get().drawText("\n");
    }
    Console::get().drawText("[i915] cpu");
    logValue("PAT", rdmsr(MSR_PAT));
    Console::get().drawText("\n");
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
    logDiagnostics();

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
