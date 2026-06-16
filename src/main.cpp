#include "Uefi/UefiBaseType.h"
#include "cpu/process/scheduler.hpp"
#include <cpu/process/exec.hpp>
#include "cpu/syscall/syscall.hpp"
#include "cpu/user/session.hpp"
#include "cpu/user/user.hpp"
#include "fs/ahci/ahci.hpp"
#include "fs/ahci/detect.hpp"
#include "fs/fat32/fat32.hpp"
#include "fs/partition/partition.hpp"
#include "fs/ramfs/ramfs.hpp"
#include <fs/storage/storage.hpp>
#include <fs/initrd/initrd.hpp>
#include <fs/devfs/devfs.hpp>
#include <iboot/memory.hpp>
#include <cpu/gdt/gdt.hpp>
#include <cpu/idt/idt.hpp>
#include <cpu/apic/irqs.hpp>
#include <memory/pmm.hpp>
#include <memory/heap.hpp>
#include <memory/vmm.hpp>
#include <graphics/framebuffer.hpp>
#include <graphics/gpu.hpp>
#include <graphics/console.hpp>
#include <cpu/acpi/acpi.hpp>
#include <cpu/acpi/pci_bus.hpp>
#include <cpu/apic/apic.hpp>
#include <cpu/apic/pic.hpp>
#include <cpu/cpuid.hpp>
#include <common/string.hpp>
#include <debug/diag.hpp>
#include <drivers/hid/i2c_hid.hpp>
#include <drivers/usb/ohci.hpp>
#include <drivers/usb/xhci.hpp>
#include <interrupts/keyboard.hpp>
#include <interrupts/timer.hpp>
#include <time/time.hpp>
#include <graphics/virtio_gpu.hpp>
#include <graphics/venus.hpp>
#include <graphics/intel_gen9.hpp>
unsigned long long runtimeBase;

namespace {
#if !defined(INSTANTOS_DEBUG) && !defined(INPUT_PROBE_ONLY)
#define INSTANTOS_BOOT_SPINNER 1
#endif

struct TableRegister {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#ifdef INSTANTOS_BOOT_SPINNER
class BootSpinner {
public:
    explicit BootSpinner(iFramebuffer& framebufferVal)
        : framebuffer(framebufferVal), frame(0) {}

    void start() {
        Console::get().setFramebufferOutputEnabled(false);
        render();
    }

    void step() {
        ++frame;
        render();
    }

    void finish() {
        framebuffer.clear(0);
        flushIfNeeded();
        Console::get().setFramebufferOutputEnabled(true);
    }

private:
    static constexpr int kDotCount = 12;
    static constexpr int kRadius = 34;
    static constexpr int kDotRadius = 4;
    static constexpr int kClearRadius = kRadius + kDotRadius + 4;

    iFramebuffer& framebuffer;
    uint32_t frame;

    void fillCircle(int centerX, int centerY, int radius, Color color) {
        static constexpr int kSubpixelScale = 8;
        static constexpr int kSampleCount = 16;
        static constexpr int kSampleOffsets[4] = { -3, -1, 1, 3 };
        const int scaledRadius = radius * kSubpixelScale;
        const int scaledRadiusSquared = scaledRadius * scaledRadius;
        const int extent = radius + 1;

        for (int y = -extent; y <= extent; ++y) {
            for (int x = -extent; x <= extent; ++x) {
                int coverage = 0;
                for (int sampleY = 0; sampleY < 4; ++sampleY) {
                    for (int sampleX = 0; sampleX < 4; ++sampleX) {
                        const int scaledX = x * kSubpixelScale + kSampleOffsets[sampleX];
                        const int scaledY = y * kSubpixelScale + kSampleOffsets[sampleY];
                        if (scaledX * scaledX + scaledY * scaledY <= scaledRadiusSquared) {
                            ++coverage;
                        }
                    }
                }

                if (coverage == 0) {
                    continue;
                }

                const int pixelX = centerX + x;
                const int pixelY = centerY + y;
                if (pixelX >= 0 && pixelY >= 0) {
                    const Color blended(
                        static_cast<uint8_t>((color.r * coverage) / kSampleCount),
                        static_cast<uint8_t>((color.g * coverage) / kSampleCount),
                        static_cast<uint8_t>((color.b * coverage) / kSampleCount)
                    );
                    framebuffer.putPixel(static_cast<uint64_t>(pixelX),
                                         static_cast<uint64_t>(pixelY),
                                         blended);
                }
            }
        }
    }

    Color dotColor(int age) const {
        const uint8_t value = static_cast<uint8_t>(80 + ((kDotCount - age) * 175) / kDotCount);
        return Color(value, value, value);
    }

    void clearArea(int centerX, int centerY) {
        for (int y = -kClearRadius; y <= kClearRadius; ++y) {
            for (int x = -kClearRadius; x <= kClearRadius; ++x) {
                const int pixelX = centerX + x;
                const int pixelY = centerY + y;
                if (pixelX >= 0 && pixelY >= 0) {
                    framebuffer.putPixel(static_cast<uint64_t>(pixelX),
                                         static_cast<uint64_t>(pixelY),
                                         0);
                }
            }
        }
    }

    void render() {
        static constexpr int offsets[kDotCount][2] = {
            { 0, -34 }, { 17, -29 }, { 29, -17 }, { 34, 0 },
            { 29, 17 }, { 17, 29 }, { 0, 34 }, { -17, 29 },
            { -29, 17 }, { -34, 0 }, { -29, -17 }, { -17, -29 },
        };

        const int centerX = static_cast<int>(framebuffer.getWidth() / 2);
        const int centerY = static_cast<int>(framebuffer.getHeight() / 2);
        clearArea(centerX, centerY);

        const int active = static_cast<int>(frame % kDotCount);
        for (int i = 0; i < kDotCount; ++i) {
            int age = active - i;
            if (age < 0) {
                age += kDotCount;
            }
            fillCircle(centerX + offsets[i][0],
                       centerY + offsets[i][1],
                       kDotRadius,
                       dotColor(age));
        }

        flushIfNeeded();
    }

    void flushIfNeeded() {
        VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();
        if (gpu.isInitialized()) {
            gpu.flush(0, 0, framebuffer.getWidth(), framebuffer.getHeight());
        }
    }
};

BootSpinner* activeBootSpinner = nullptr;

void tickBootSpinner() {
    if (activeBootSpinner) {
        activeBootSpinner->step();
    }
}
#else
void tickBootSpinner() {}
#endif

void drawHexLine(const char* label, uint64_t value) {
    Console::get().drawText(label);
    Console::get().drawHex(value);
    Console::get().drawText("\n");
}

void drawTextLine(const char* text) {
    Console::get().drawText(text);
    Console::get().drawText("\n");
}

const char* virtioIrqModeName(VirtIOGPUDriver::IRQMode mode) {
    switch (mode) {
        case VirtIOGPUDriver::IRQMode::LegacyINTx:
            return "INTx";
        case VirtIOGPUDriver::IRQMode::MSI:
            return "MSI";
        case VirtIOGPUDriver::IRQMode::None:
        default:
            return "none";
    }
}

void drawBoolLine(const char* label, bool value) {
    Console::get().drawText(label);
    Console::get().drawText(value ? "yes" : "no");
    Console::get().drawText("\n");
}

void logBootText(const char* text) {
    Console::get().drawText(text);
    Cereal::get().write(text);
}

void logBootNumber(uint64_t value) {
    Console::get().drawNumber(static_cast<int64_t>(value));
    char buffer[21];
    int pos = 0;
    if (value == 0) {
        Cereal::get().write('0');
        return;
    }
    while (value > 0 && pos < static_cast<int>(sizeof(buffer))) {
        buffer[pos++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    while (pos > 0) {
        Cereal::get().write(buffer[--pos]);
    }
}

bool hasAnyKeyboard() {
    return Keyboard::get().isKeyboardPresent() ||
        USBInput::get().hasKeyboard() ||
        I2CHIDController::get().hasKeyboard();
}

void pollInputBackends() {
    Keyboard::get().servicePendingInput();
    USBInput::get().poll();
    I2CHIDController::get().poll();
}

bool waitForKeyboardProbe(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; ++i) {
        if (hasAnyKeyboard()) {
            return true;
        }
        pollInputBackends();
        if ((i % 50000) == 0) {
            tickBootSpinner();
        }
        asm volatile("pause");
    }
    return hasAnyKeyboard();
}

void drawKeyboardStatus(const char* prefix) {
    logBootText(prefix);
    logBootText(" ps2.driver=");
    logBootText(Keyboard::get().isInitialized() ? "yes" : "no");
    logBootText(" ps2.keyboard=");
    logBootText(Keyboard::get().isKeyboardPresent() ? "yes" : "no");
    logBootText(" usb.controller=");
    logBootText(USBInput::get().hasController() ? "yes" : "no");
    logBootText(" usb.xhci=");
    logBootText(USBInput::get().isXhciActive() ? "yes" : "no");
    logBootText(" usb.xhci.controllers=");
    logBootNumber(XHCIController::get().controllerCount());
    logBootText("/");
    logBootNumber(XHCIController::get().initializedControllerCount());
    logBootText(" usb.keyboard=");
    logBootText(USBInput::get().hasKeyboard() ? "yes" : "no");
    logBootText(" i2c.controllers=");
    logBootNumber(I2CHIDController::get().getControllerCount());
    logBootText(" i2c.hints=");
    logBootNumber(I2CHIDController::get().getHidHintCount());
    logBootText(" i2c.keyboard=");
    logBootText(I2CHIDController::get().hasKeyboard() ? "yes" : "no");
    logBootText("\n");
}

void drawKeyboardFailureHints() {
    logBootText("[input] detail: xhci.controllers=");
    logBootNumber(XHCIController::get().controllerCount());
    logBootText(" initialized=");
    logBootNumber(XHCIController::get().initializedControllerCount());
    logBootText(" usb.backend=");
    if (!USBInput::get().hasController()) {
        logBootText("none");
    } else if (USBInput::get().isXhciActive()) {
        logBootText("xhci");
    } else {
        logBootText("ohci");
    }
    logBootText(" ps2=");
    logBootText(Keyboard::get().isKeyboardPresent() ? "detected" : "missing");
    logBootText(" usb=");
    logBootText(USBInput::get().hasKeyboard() ? "detected" : "missing");
    logBootText(" i2c.devices=");
    logBootNumber(I2CHIDController::get().getDeviceCount());
    logBootText("\n");

    logBootText("[input] hint: if xhci.controllers>0 but usb.keyboard=no, check the on-screen [usb:xhci] logs for the bus/port that failed enumeration\n");
}

void waitForKeyboardBeforeBoot() {
    // Real hardware can surface USB/I2C keyboards a little after controller init.
    // Give the input backends a short chance to settle before showing a warning.
    if (waitForKeyboardProbe(500000)) {
        drawKeyboardStatus("[input] keyboard detected:");
        return;
    }

    Console::get().setTextColor(0xFFDD66);
    logBootText("[input] no keyboard detected after initial probe; continuing boot with diagnostics enabled\n");
    Console::get().setTextColor(0xFFFFFF);
    drawKeyboardStatus("[input] state:");
    drawKeyboardFailureHints();

    uint32_t polls = 0;
    while (!hasAnyKeyboard() && polls < 2000000) {
        pollInputBackends();
        if ((polls % 50000) == 0) {
            tickBootSpinner();
        }
        ++polls;
        asm volatile("pause");
    }

    if (hasAnyKeyboard()) {
        drawKeyboardStatus("[input] keyboard detected:");
    } else {
        Console::get().setTextColor(0xFFDD66);
        logBootText("[input] proceeding without a detected keyboard; use serial logs or input-probe ISO for driver debugging\n");
        Console::get().setTextColor(0xFFFFFF);
    }
}

void dumpVirtIOGPUCaps(VirtIOGPUDriver& virtioGpu) {
    drawBoolLine("[VGPU] virgl=", virtioGpu.supportsVirgl());
    drawBoolLine("[VGPU] context_init=", virtioGpu.supportsContextInit());
    drawBoolLine("[VGPU] resource_blob=", virtioGpu.supportsBlobResources());
    drawBoolLine("[VGPU] resource_uuid=", virtioGpu.supportsResourceUUID());
    drawBoolLine("[VGPU] scanout_blob_active=", virtioGpu.isUsingBlobScanout());
    drawBoolLine("[VGPU] venus=", venus::Venus::get().negotiate());

    Console::get().drawText("[VGPU] capsets=");
    Console::get().drawNumber(static_cast<int64_t>(virtioGpu.getNumCapsets()));
    Console::get().drawText("\n");

    for (uint32_t i = 0; i < virtioGpu.getNumCapsets(); ++i) {
        VirtIOGPUCapsetInfo info = {};
        if (!virtioGpu.getCapsetInfo(i, &info)) {
            Console::get().drawText("[VGPU] capset[");
            Console::get().drawNumber(static_cast<int64_t>(i));
            Console::get().drawText("]: query failed\n");
            continue;
        }

        Console::get().drawText("[VGPU] capset[");
        Console::get().drawNumber(static_cast<int64_t>(i));
        Console::get().drawText("]: id=");
        Console::get().drawNumber(static_cast<int64_t>(info.capset_id));
        Console::get().drawText(" version=");
        Console::get().drawNumber(static_cast<int64_t>(info.capset_max_version));
        Console::get().drawText(" size=");
        Console::get().drawNumber(static_cast<int64_t>(info.capset_max_size));
        Console::get().drawText("\n");
    }
}

void dumpVirtIOGPUFences(VirtIOGPUDriver& virtioGpu) {
    Console::get().drawText("[VGPU] fences submitted=");
    Console::get().drawHex(virtioGpu.getLastSubmittedFence());
    Console::get().drawText(" completed=");
    Console::get().drawHex(virtioGpu.getLastCompletedFence());
    Console::get().drawText(" last_resp=");
    Console::get().drawText(VirtIOGPUDriver::describeResponseType(virtioGpu.getLastResponseType()));
    Console::get().drawText("\n");

    Console::get().drawText("[VGPU] irq queue=");
    Console::get().drawNumber(static_cast<int64_t>(virtioGpu.getQueueInterruptCount()));
    Console::get().drawText(" config=");
    Console::get().drawNumber(static_cast<int64_t>(virtioGpu.getConfigInterruptCount()));
    Console::get().drawText(" total=");
    Console::get().drawNumber(static_cast<int64_t>(virtioGpu.getInterruptCount()));
    Console::get().drawText("\n");
}

void runVirtIOGPUProbe(VirtIOGPUDriver& virtioGpu) {
    if (!virtioGpu.supportsVirgl()) {
        return;
    }

    VirtIOGPUVirglProbeResult probe = {};
    const bool ok = virtioGpu.runVirglProbe(&probe);
    Console::get().drawText("[VGPU] probe: ");
    Console::get().drawText(ok ? "transport-ok" : "incomplete");
    Console::get().drawText("\n");

    Console::get().drawText("[VGPU] probe capset id=");
    Console::get().drawNumber(static_cast<int64_t>(probe.capsetId));
    Console::get().drawText(" version=");
    Console::get().drawNumber(static_cast<int64_t>(probe.capsetVersion));
    Console::get().drawText(" size=");
    Console::get().drawNumber(static_cast<int64_t>(probe.capsetSize));
    Console::get().drawText("\n");

    Console::get().drawText("[VGPU] probe stages: capset-info=");
    Console::get().drawText(probe.capsetInfoOk ? "ok" : "fail");
    Console::get().drawText(" capset=");
    Console::get().drawText(probe.capsetFetchOk ? "ok" : "fail");
    Console::get().drawText(" ctx=");
    Console::get().drawText(probe.contextCreateOk ? "ok" : "fail");
    Console::get().drawText(" res=");
    Console::get().drawText(probe.resourceCreateOk ? "ok" : "fail");
    Console::get().drawText(" attach=");
    Console::get().drawText(probe.resourceAttachOk ? "ok" : "fail");
    Console::get().drawText(" submit=");
    Console::get().drawText(probe.submitTransportOk ? "ok" : "fail");
    Console::get().drawText(" fence=");
    Console::get().drawText(probe.fenceCompleted ? "ok" : "fail");
    Console::get().drawText("\n");

    Console::get().drawText("[VGPU] probe response=");
    Console::get().drawText(VirtIOGPUDriver::describeResponseType(probe.responseType));
    Console::get().drawText(" submitted=");
    Console::get().drawHex(probe.submittedFence);
    Console::get().drawText(" completed=");
    Console::get().drawHex(probe.completedFence);
    Console::get().drawText("\n");
}

// Probe for an Intel HD Graphics 530 / Skylake (Gen9) integrated GPU and report
// what was found. Output is prefixed with [i915] so smoke tests can grep the
// serial log. This is non-fatal and harmless on hosts without an Intel iGPU
// (e.g. QEMU): it simply reports "not present" and boot continues on virtio.
void reportIntelGen9() {
    intel_gen9::IntelGen9Driver& drv = intel_gen9::IntelGen9Driver::get();
    if (!drv.probe()) {
        Console::get().drawText("[i915] Intel Gen9 GPU: not present\n");
        return;
    }
    const intel_gen9::Gen9DeviceInfo* dev = drv.deviceInfo();
    Console::get().drawText("[i915] Intel Gen9 GPU detected: ");
    Console::get().drawText(dev ? dev->name : "unknown");
    Console::get().drawText(dev && dev->isHd530 ? " [HD 530]\n" : "\n");
}

// Bring up Venus (Vulkan over virtio-gpu) and run a synchronous
// vkEnumerateInstanceVersion round trip against the host renderer. All output
// is prefixed with [VENUS] so smoke tests can grep the serial log.
void runVenusProbe(VirtIOGPUDriver& virtioGpu) {
    venus::Venus& vk = venus::Venus::get();
    if (!vk.negotiate()) {
        Console::get().drawText("[VENUS] capset: unavailable (host renderer not in venus mode)\n");
        return;
    }

    venus::VenusProbeResult probe = {};
    const bool ok = vk.probe(&probe);

    Console::get().drawText("[VENUS] probe: ");
    Console::get().drawText(ok ? "ok" : "incomplete");
    Console::get().drawText("\n");

    Console::get().drawText("[VENUS] capset wire=");
    Console::get().drawNumber(static_cast<int64_t>(probe.wireFormatVersion));
    Console::get().drawText(" vk_xml=");
    Console::get().drawHex(probe.vkXmlVersion);
    Console::get().drawText(" protocol=");
    Console::get().drawNumber(static_cast<int64_t>(probe.venusProtocolVersion));
    Console::get().drawText("\n");

    Console::get().drawText("[VENUS] stages: capset=");
    Console::get().drawText(probe.capsetVersionOk ? "ok" : "fail");
    Console::get().drawText(" ctx=");
    Console::get().drawText(probe.contextOk ? "ok" : "fail");
    Console::get().drawText(" blob=");
    Console::get().drawText(probe.blobOk ? "ok" : "fail");
    Console::get().drawText(" submit=");
    Console::get().drawText(probe.submitOk ? "ok" : "fail");
    Console::get().drawText(" fence=");
    Console::get().drawText(probe.fenceOk ? "ok" : "fail");
    Console::get().drawText(" reply=");
    Console::get().drawText(probe.replyOk ? "ok" : "fail");
    Console::get().drawText("\n");

    if (probe.replyOk) {
        const uint32_t v = probe.instanceVersion;
        Console::get().drawText("[VENUS] vkEnumerateInstanceVersion=");
        Console::get().drawNumber(static_cast<int64_t>((v >> 22) & 0x7F));
        Console::get().drawText(".");
        Console::get().drawNumber(static_cast<int64_t>((v >> 12) & 0x3FF));
        Console::get().drawText(".");
        Console::get().drawNumber(static_cast<int64_t>(v & 0xFFF));
        Console::get().drawText(" (raw=");
        Console::get().drawHex(v);
        Console::get().drawText(")\n");
    }

    Console::get().drawText("[VENUS] response=");
    Console::get().drawText(VirtIOGPUDriver::describeResponseType(probe.responseType));
    Console::get().drawText("\n");

    // Fuller Vulkan bring-up over the async ring: instance, physical devices,
    // properties, and a logical device.
    venus::VenusVulkanResult vkr = {};
    const bool vkOk = vk.bringUpVulkan(&vkr);
    Console::get().drawText("[VENUS] vulkan: ");
    Console::get().drawText(vkOk ? "ok" : "incomplete");
    Console::get().drawText("\n");

    Console::get().drawText("[VENUS] vk stages: ring=");
    Console::get().drawText(vkr.ringOk ? "ok" : "fail");
    Console::get().drawText(" instance=");
    Console::get().drawText(vkr.instanceOk ? "ok" : "fail");
    Console::get().drawText(" phys_devs=");
    Console::get().drawNumber(static_cast<int64_t>(vkr.physDevCount));
    Console::get().drawText(" props=");
    Console::get().drawText(vkr.propsOk ? "ok" : "fail");
    Console::get().drawText(" device=");
    Console::get().drawText(vkr.deviceOk ? "ok" : "fail");
    Console::get().drawText(" compute=");
    Console::get().drawText(vkr.computeOk ? "ok" : "fail");
    Console::get().drawText("\n");

    if (vkr.deviceOk) {
        Console::get().drawText("[VENUS] compute: elements=");
        Console::get().drawNumber(static_cast<int64_t>(vkr.computeElements));
        Console::get().drawText(" mismatches=");
        Console::get().drawNumber(static_cast<int64_t>(vkr.computeMismatches));
        Console::get().drawText(" data[3]=");
        Console::get().drawNumber(static_cast<int64_t>(vkr.computeSample));
        Console::get().drawText(" (expect 10)\n");
    }

    if (vkr.propsOk) {
        const uint32_t a = vkr.device0.apiVersion;
        Console::get().drawText("[VENUS] gpu0 name='");
        Console::get().drawText(vkr.device0.deviceName);
        Console::get().drawText("' type=");
        Console::get().drawNumber(static_cast<int64_t>(vkr.device0.deviceType));
        Console::get().drawText(" vendor=");
        Console::get().drawHex(vkr.device0.vendorId);
        Console::get().drawText(" api=");
        Console::get().drawNumber(static_cast<int64_t>((a >> 22) & 0x7F));
        Console::get().drawText(".");
        Console::get().drawNumber(static_cast<int64_t>((a >> 12) & 0x3FF));
        Console::get().drawText(".");
        Console::get().drawNumber(static_cast<int64_t>(a & 0xFFF));
        Console::get().drawText("\n");
    }
    if (vkr.instanceOk) {
        Console::get().drawText("[VENUS] instance=");
        Console::get().drawHex(vkr.instanceHandle);
        Console::get().drawText(" device=");
        Console::get().drawHex(vkr.deviceHandle);
        Console::get().drawText("\n");
    }

    // Visual demo: render a real GPU triangle (graphics pipeline) and blit it to
    // the display so it is actually visible on screen. Held briefly so it can be
    // seen / screenshotted before the boot continues into the compositor.
    const bool drew = vk.renderTriangleToScreen(480);
    Console::get().drawText("[VENUS] triangle on screen: ");
    Console::get().drawText(drew ? "ok" : "fail");
    Console::get().drawText("\n");
    if (drew) {
        // Hold the rendered frame ~3s (busy wait on the PIT/TSC-independent loop)
        // so the GPU triangle is visible. Re-flush periodically in case the
        // console scrolls over it.
        for (volatile uint64_t i = 0; i < 6000000000ULL; ++i) {
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
}

// Write a small text file via the VFS (used to seed /etc/passwd, /etc/group).
// Creates/truncates the file and writes the contents; best-effort.
void writeSystemFile(const char* path, const char* contents) {
    FileDescriptor* fd = nullptr;
    // O_WRONLY | O_CREAT | O_TRUNC == 0x1 | 0100 | 01000
    if (VFS::get().open(path, 0x1 | 0100 | 01000, &fd, 0644) != 0 || !fd) {
        return;
    }
    uint64_t len = 0;
    while (contents[len]) len++;
    if (len > 0) {
        VFS::get().write(fd, contents, len);
    }
    VFS::get().close(fd);
}

// Mirror of struct utmpx (abi-bits/utmpx.h) used for the login records below;
// see writeUtmpRecord, which writes the 400-byte layout at fixed offsets.

// Write /var/run/utmp with login records (BOOT_TIME, USER_PROCESS) so
// uptime/who/users have data. The record is written at fixed byte offsets to
// exactly match struct utmpx (abi-bits/utmpx.h), which is 400 bytes:
//   off 0   short ut_type
//   off 4   int   ut_pid
//   off 8   char  ut_line[32]
//   off 40  char  ut_id[4]
//   off 44  char  ut_user[32]
//   off 76  char  ut_host[256]
//   off 344 long  ut_tv.tv_sec    (8-aligned after ut_exit/session/pad)
//   off 352 long  ut_tv.tv_usec
void writeUtmpRecord(short type, const char* user, const char* line, uint64_t unixTime) {
    uint8_t rec[400];
    for (unsigned i = 0; i < sizeof(rec); ++i) rec[i] = 0;

    auto put16 = [&](unsigned off, uint16_t v) {
        rec[off] = v & 0xFF; rec[off + 1] = (v >> 8) & 0xFF;
    };
    auto put64 = [&](unsigned off, uint64_t v) {
        for (int i = 0; i < 8; ++i) rec[off + i] = (v >> (i * 8)) & 0xFF;
    };
    auto putStr = [&](unsigned off, unsigned cap, const char* s) {
        unsigned i = 0;
        for (; s && s[i] && i + 1 < cap; ++i) rec[off + i] = (uint8_t)s[i];
    };

    put16(0, (uint16_t)type);   // ut_type
    putStr(8, 32, line);        // ut_line
    rec[40] = '~'; rec[41] = '~';  // ut_id
    putStr(44, 32, user);       // ut_user
    put64(344, unixTime);       // ut_tv.tv_sec
    put64(352, 0);              // ut_tv.tv_usec

    FileDescriptor* fd = nullptr;
    // O_WRONLY | O_CREAT | O_APPEND == 0x1 | 0100 | 02000
    if (VFS::get().open("/var/run/utmp", 0x1 | 0100 | 02000, &fd, 0644) != 0 || !fd) {
        return;
    }
    VFS::get().write(fd, rec, sizeof(rec));
    VFS::get().close(fd);
}

void launchInitrdService(const char* binaryPath) {
    Process* process = ProcessExecutor::loadUserBinary(binaryPath);
    Console::get().drawText(binaryPath);
    Console::get().drawText(": [ ");
    Console::get().setTextColor(process ? 0x49ceee : 0xFF0000);
    Console::get().drawText(process ? "OK" : "FAIL");
    Console::get().setTextColor(0xFFFFFF);
    Console::get().drawText(" ]\n");

    if (process) {
        Scheduler::get().addProcess(process);
    }
    tickBootSpinner();
}
}

static void printMemoryDiagnostics(size_t heapSize) {
    HeapStats stats = heap_stats();

    Console::get().drawText("[MEM] PMM free frames: ");
    Console::get().drawNumber(static_cast<int64_t>(PMM::FreeFrameCount()));
    Console::get().drawText(" (");
    Console::get().drawNumber(static_cast<int64_t>(PMM::FreeMemory() / (1024 * 1024)));
    Console::get().drawText(" MiB)\n");

    Console::get().drawText("[MEM] Heap size: ");
    Console::get().drawNumber(static_cast<int64_t>(heapSize));
    Console::get().drawText(" bytes, allocated: ");
    Console::get().drawNumber(static_cast<int64_t>(stats.total_allocated));
    Console::get().drawText(" bytes, peak: ");
    Console::get().drawNumber(static_cast<int64_t>(stats.peak_usage));
    Console::get().drawText(" bytes, free blocks: ");
    Console::get().drawNumber(static_cast<int64_t>(stats.free_block_count));
    Console::get().drawText("\n");
}

#ifdef INPUT_PROBE_ONLY
static void runInputProbeOnly() {
    Console::get().drawText("[input-probe] results\n");
    drawBoolLine("[input-probe] ps2.keyboard=", Keyboard::get().isInitialized());
    drawBoolLine("[input-probe] ps2.mouse=", Keyboard::get().isMouseEnabled());
    drawBoolLine("[input-probe] usb.controller=", USBInput::get().hasController());
    drawBoolLine("[input-probe] usb.xhci=", USBInput::get().isXhciActive());
    drawBoolLine("[input-probe] usb.keyboard=", USBInput::get().hasKeyboard());
    drawBoolLine("[input-probe] usb.mouse=", USBInput::get().hasMouse());
    drawBoolLine("[input-probe] i2c.controller=", I2CHIDController::get().getControllerCount() > 0);
    drawBoolLine("[input-probe] i2c.hid-acpi=", I2CHIDController::get().getHidHintCount() > 0);
    drawBoolLine("[input-probe] i2c.hid-device=", I2CHIDController::get().getDeviceCount() > 0);
    drawBoolLine("[input-probe] i2c.keyboard=", I2CHIDController::get().hasKeyboard());
    drawBoolLine("[input-probe] i2c.mouse=", I2CHIDController::get().hasMouse());
    Console::get().drawText("[input-probe] i2c.controllers=");
    Console::get().drawNumber(I2CHIDController::get().getControllerCount());
    Console::get().drawText(" hints=");
    Console::get().drawNumber(I2CHIDController::get().getHidHintCount());
    Console::get().drawText(" devices=");
    Console::get().drawNumber(I2CHIDController::get().getDeviceCount());
    Console::get().drawText("\n[input-probe] done; halting after input polling\n");

    asm volatile("sti");
    while (true) {
        Keyboard::get().servicePendingInput();
        USBInput::get().poll();
        I2CHIDController::get().poll();
        asm volatile("hlt");
    }
}
#endif

extern "C" void InstantOS(BootInfo* bootInfo) {
    runtimeBase = bootInfo->kernelBase;
    asm("cli");
    if (!bootInfo->framebuffer.base || bootInfo->framebuffer.format == PixelFormat::BltOnly) {
        while(1) {
            asm("hlt");
        }
    }
    int totalMemory = bootInfo->memoryMap.totalBytes / (1024 * 1024);

    
    iFramebuffer fb(&bootInfo->framebuffer);
    Console::get().initialize(&fb);
    Console::get().setBackgroundColor(0);
    Console::get().setTextColor(0xFFFFFF);
    fb.clear(0);
#ifdef INSTANTOS_BOOT_SPINNER
    BootSpinner bootSpinner(fb);
    activeBootSpinner = &bootSpinner;
    bootSpinner.start();
#endif

    GDT::get().initialize();
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif
    
    if (!CPU::initialize()) {
        while (1) {
            asm("hlt");
        }
    }
    memory_init_acceleration();
    if (!memory_validate_acceleration()) {
        Console::get().drawText("Memory acceleration: [ ");
        Console::get().setTextColor(0xFF0000);
        Console::get().drawText("FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
        while (1) {
            asm("hlt");
        }
    }
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif
    
    PMM::Initialize(bootInfo->memoryMap, bootInfo->kernelBase, bootInfo->kernelSize);
    if (bootInfo->framebuffer.base && bootInfo->framebuffer.size) {
        PMM::ReserveRange(bootInfo->framebuffer.base, bootInfo->framebuffer.size);
    }
    PMM::DumpReservations();
    if (bootInfo->initrdBase && bootInfo->initrdSize) {
        PMM::ReserveRange(bootInfo->initrdBase, bootInfo->initrdSize);

        uint64_t initrdStart = bootInfo->initrdBase & ~(PMM::PAGE_SIZE - 1);
        uint64_t initrdEnd = bootInfo->initrdBase + bootInfo->initrdSize;
        if (initrdEnd < bootInfo->initrdBase || initrdEnd > UINT64_MAX - (PMM::PAGE_SIZE - 1)) {
            initrdEnd = UINT64_MAX & ~(PMM::PAGE_SIZE - 1);
        } else {
            initrdEnd = (initrdEnd + PMM::PAGE_SIZE - 1) & ~(PMM::PAGE_SIZE - 1);
        }
        uint64_t initrdPages = (initrdEnd - initrdStart) / PMM::PAGE_SIZE;

        Console::get().drawText("PMM: reserved initrd frames: ");
        Console::get().drawNumber(static_cast<int64_t>(initrdPages));
        Console::get().drawText("\n");
    }
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif
    
    VMM::Initialize();
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif
    
    size_t heapSize = 0x1000000; // 16 MiB initial heap
    uint64_t heapPages = (heapSize + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    void* heapBase = reinterpret_cast<void*>(PMM::AllocFrames(heapPages));
    if (!heapBase) {
        Console::get().drawText("Heap: [ ");
        Console::get().setTextColor(0xFF0000);
        Console::get().drawText("FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
        while (1) {
            asm("hlt");
        }
    }
    heap_init(heapBase, heapSize);
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif

    if(!ACPI::get().initialize(bootInfo->rsdp)) {
        Console::get().drawText("ACPI: [ ");
        Console::get().setTextColor(0xFF0000);
        Console::get().drawText("FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
    }
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif

    IDT::get();
    PIC::disable();

    if(!APICManager::get().initialize()) {
        Console::get().drawText("APIC: [ ");
        Console::get().setTextColor(0xFF0000);
        Console::get().drawText("FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
    }
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif
    
    APICManager& apic = APICManager::get();
    uint8_t targetCore = static_cast<uint8_t>(LAPIC::get().getId());

    drawTextLine("[BOOT] before mapIRQ timer");
    apic.mapIRQ(IRQ_TIMER, VECTOR_TIMER, targetCore);
    drawTextLine("[BOOT] after mapIRQ timer");
    Scheduler::get().initialize();
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif
    
    ISR::registerIRQ(VECTOR_TIMER, &Timer::get());
    if (!time_init()) {
        Console::get().drawText("Time: [ ");
        Console::get().setTextColor(0xFF0000);
        Console::get().drawText("FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
        while (1) {
            asm volatile("hlt");
        }
    }
    ISR::registerIRQ(VECTOR_KEYBOARD, &Keyboard::get());
    ISR::registerIRQ(VECTOR_MOUSE, &Keyboard::get());
    drawTextLine("[BOOT] before mapIRQ keyboard");
    apic.mapIRQ(IRQ_KEYBOARD, VECTOR_KEYBOARD, targetCore);
    drawTextLine("[BOOT] after mapIRQ keyboard");
    drawTextLine("[BOOT] before mapIRQ mouse");
    apic.mapIRQ(IRQ_MOUSE, VECTOR_MOUSE, targetCore);
    drawTextLine("[BOOT] after mapIRQ mouse");
    drawTextLine("[BOOT] before PCI bus scan");
    {
        const size_t pciDevices = PciBus::get().scan();
        drawHexLine("[BOOT] PCI devices=", pciDevices);
    }
    USBInput::get().initialize();
    I2CHIDController::get().initialize();
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif

#ifndef INPUT_PROBE_ONLY
    waitForKeyboardBeforeBoot();
#endif
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif

#ifdef INPUT_PROBE_ONLY
    runInputProbeOnly();
#endif

    GPU& gpu = GPU::get();
    VirtIOGPUDriver& virtioGpu = VirtIOGPUDriver::get();
    reportIntelGen9();
    Console::get().drawText("[BOOT] framebuffer fallback=");
    Console::get().drawNumber(static_cast<int64_t>(fb.getWidth()));
    Console::get().drawText("x");
    Console::get().drawNumber(static_cast<int64_t>(fb.getHeight()));
    Console::get().drawText("\n");
    if (gpu.initialize(&fb) && gpu.getActiveBackendKind() == GPUBackendKind::VirtIO) {
        uint32_t gpuWidth = 0;
        uint32_t gpuHeight = 0;
        virtioGpu.getMode(&gpuWidth, &gpuHeight);
        Console::get().setVirtIO(true);
        Console::get().setCopyScrollEnabled(true);
        fb.clear(0);
        gpu.flush(0, 0, gpuWidth, gpuHeight);
#ifdef INSTANTOS_BOOT_SPINNER
        bootSpinner.step();
#endif
        Console::get().drawText("[BOOT] VirtIO GPU: [ OK ]\n");
        Console::get().drawText("[BOOT] VirtIO GPU IRQ mode: ");
        Console::get().drawText(virtioIrqModeName(virtioGpu.getIRQMode()));
        Console::get().drawText(", vector=");
        Console::get().drawHex(virtioGpu.getIRQVector());
        if (virtioGpu.getIRQMode() == VirtIOGPUDriver::IRQMode::LegacyINTx) {
            Console::get().drawText(", line=");
            Console::get().drawNumber(virtioGpu.getIRQLine());
        }
        Console::get().drawText(", irq_registered=");
        Console::get().drawText(virtioGpu.getIRQMode() == VirtIOGPUDriver::IRQMode::None ? "no" : "yes");
        Console::get().drawText("\n");
        dumpVirtIOGPUCaps(virtioGpu);
        runVirtIOGPUProbe(virtioGpu);
        runVenusProbe(virtioGpu);
        dumpVirtIOGPUFences(virtioGpu);
    } else {
        Console::get().drawText("[BOOT] active GPU backend: ");
        Console::get().drawText(gpu.getActiveBackendName());
        Console::get().drawText("\n");
        if (gpu.getActiveBackendKind() == GPUBackendKind::IntelGen9) {
            Console::get().drawText("[BOOT] Intel Gen9 display: [ OK ]\n");
        } else {
            Console::get().drawText("[BOOT] VirtIO GPU: unavailable, using boot framebuffer\n");
        }
    }
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif
    
    Syscall::get().initialize();
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif

    TableRegister gdtr = {};
    TableRegister idtr = {};
    asm volatile("sgdt %0" : "=m"(gdtr));
    asm volatile("sidt %0" : "=m"(idtr));

    drawHexLine("[BOOT] LAPIC ID=", LAPIC::get().getId());
    drawHexLine("[BOOT] IRQ target APIC ID=", targetCore);
    drawHexLine("[BOOT] GDTR base=", gdtr.base);
    drawHexLine("[BOOT] IDTR base=", idtr.base);

    UserManager::get().initialize();
    SessionManager::get().initialize();
    VFS::get().initialize();
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif

    SATABlockDevice* device = nullptr;
    FAT32FS* fs = nullptr;
    KernelStorage::reset();
    Console::get().drawText("\033[2Jdetecting ahci\n");
    auto controller = AHCIDetector::detectAndInitialize();
    if (controller) {
        auto port = controller->getPort(0);
        if (port && port->isActive()) {
            static SATABlockDevice _device(port);
            device = &_device;
            KernelStorage::setDevice("ahci0", device, device->getSize(), 512, true, true);

            uint64_t partStart = 0;
            uint64_t partLength = 0;
            uint8_t partType = 0;
            if (mbrFindFatPartition(device, &partStart, &partLength, &partType)) {
                static PartitionBlockDevice _partition(device, partStart, partLength);
                static FAT32FS _fs(&_partition);
                fs = &_fs;
                int mountResult = VFS::get().mount(fs, "/");
                KernelStorage::setMount("/", "fat32", mountResult);
                if(mountResult != 0){
                    Console::get().drawText("Filesystem mounted: [ ");
                    Console::get().setTextColor(0xFF0000);
                    Console::get().drawText("FAIL");
                    Console::get().setTextColor(0xFFFFFF);
                    Console::get().drawText(" ]\n");
                }
            } else {
                KernelStorage::setMount("/", "fat32", -1);
                Console::get().drawText("Partition table: [ ");
                Console::get().setTextColor(0xFF0000);
                Console::get().drawText("FAIL");
                Console::get().setTextColor(0xFFFFFF);
                Console::get().drawText(" ]\n");
            }
        } else {
            KernelStorage::setMount("/", "fat32", -1);
            Console::get().drawText("AHCI Port: [ ");
            Console::get().setTextColor(0xFF0000);
            Console::get().drawText("FAIL");
            Console::get().setTextColor(0xFFFFFF);
            Console::get().drawText(" ]\n");
        }
    } else {
        KernelStorage::setMount("/", "fat32", -1);
        Console::get().drawText("AHCI: [ ");
        Console::get().setTextColor(0xFF0000);
        Console::get().drawText("FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
    }
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.step();
#endif
    
    {
        static DevFS devfs;
        int devMountResult = VFS::get().mount(&devfs, "/dev");
        Console::get().drawText("DevFS at /dev: [ ");
        Console::get().setTextColor(devMountResult == 0 ? 0x49ceee : 0xFF0000);
        Console::get().drawText(devMountResult == 0 ? "OK" : "FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
    }
    
    if (bootInfo->initrdBase && bootInfo->initrdSize) {
        Console::get().drawText("Mounting initrdfs...\n");

        static InitrdFS binInitrdfs(
            reinterpret_cast<void*>(bootInfo->initrdBase),
            bootInfo->initrdSize,
            "bin"
        );
        int mountResult = VFS::get().mount(&binInitrdfs, "/bin");
        Console::get().drawText("InitrdFS at /bin: [ ");
        Console::get().setTextColor(mountResult == 0 ? 0x49ceee : 0xFF0000);
        Console::get().drawText(mountResult == 0 ? "OK" : "FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");

        if (mountResult == 0) {
            static InitrdFS libInitrdfs(
                reinterpret_cast<void*>(bootInfo->initrdBase),
                bootInfo->initrdSize,
                "lib"
            );
            int libMountResult = VFS::get().mount(&libInitrdfs, "/lib");
            Console::get().drawText("InitrdFS at /lib: [ ");
            Console::get().setTextColor(libMountResult == 0 ? 0x49ceee : 0xFF0000);
            Console::get().drawText(libMountResult == 0 ? "OK" : "FAIL");
            Console::get().setTextColor(0xFFFFFF);
            Console::get().drawText(" ]\n");

            // Mount the initrd's system include headers (used by tcc and any
            // hosted compiler) at /include, matching tcc's sysinclude search path.
            static InitrdFS includeInitrdfs(
                reinterpret_cast<void*>(bootInfo->initrdBase),
                bootInfo->initrdSize,
                "include"
            );
            int includeMountResult = VFS::get().mount(&includeInitrdfs, "/include");
            Console::get().drawText("InitrdFS at /include: [ ");
            Console::get().setTextColor(includeMountResult == 0 ? 0x49ceee : 0xFF0000);
            Console::get().drawText(includeMountResult == 0 ? "OK" : "FAIL");
            Console::get().setTextColor(0xFFFFFF);
            Console::get().drawText(" ]\n");

            // Mount the NetSurf browser resource tree (HTML/CSS/images/fonts
            // bundled under "netsurf/" in the initrd) at /netsurf, matching the
            // path NetSurf is compiled with (NETSURF_FRAMEBUFFER_RESOURCES=
            // /netsurf/res/). Without this mount the browser cannot find its
            // stylesheets and the welcome page renders unstyled / fails.
            static InitrdFS netsurfInitrdfs(
                reinterpret_cast<void*>(bootInfo->initrdBase),
                bootInfo->initrdSize,
                "netsurf"
            );
            int netsurfMountResult = VFS::get().mount(&netsurfInitrdfs, "/netsurf");
            Console::get().drawText("InitrdFS at /netsurf: [ ");
            Console::get().setTextColor(netsurfMountResult == 0 ? 0x49ceee : 0xFF0000);
            Console::get().drawText(netsurfMountResult == 0 ? "OK" : "FAIL");
            Console::get().setTextColor(0xFFFFFF);
            Console::get().drawText(" ]\n");

            // A writable in-memory /tmp (tmpfs). RamFS supports the full Unix
            // surface (ownership, mknod/mkfifo, etc.) that FAT32 cannot.
            static RamFS tmpfs;
            int tmpMountResult = VFS::get().mount(&tmpfs, "/tmp");
            Console::get().drawText("RamFS at /tmp: [ ");
            Console::get().setTextColor(tmpMountResult == 0 ? 0x49ceee : 0xFF0000);
            Console::get().drawText(tmpMountResult == 0 ? "OK" : "FAIL");
            Console::get().setTextColor(0xFFFFFF);
            Console::get().drawText(" ]\n");

            // A small in-memory /etc holding the user/group databases so libc's
            // getpwuid()/getgrgid() resolve numeric ids to names (id, whoami,
            // ls -l). RamFS is always available (no disk dependency).
            static RamFS etcfs;
            int etcMountResult = VFS::get().mount(&etcfs, "/etc");
            if (etcMountResult == 0) {
                writeSystemFile("/etc/passwd",
                    "root:x:0:0:root:/:/bin/bash\n");
                writeSystemFile("/etc/group",
                    "root:x:0:\n");
                writeSystemFile("/etc/hostname",
                    "instantos\n");
                // Resolver configuration for mlibc's built-in DNS stub.
                // QEMU user-mode networking exposes a DNS forwarder at
                // 10.0.2.3; the resolver reads the first nameserver line.
                writeSystemFile("/etc/resolv.conf",
                    "nameserver 10.0.2.3\n");
                // /etc/services: the DNS resolver looks up the "domain"
                // service to obtain UDP port 53. Without this entry
                // getaddrinfo() fails with EAI_SERVICE.
                writeSystemFile("/etc/services",
                    "domain\t53/tcp\n"
                    "domain\t53/udp\n"
                    "http\t80/tcp\n"
                    "https\t443/tcp\n");
                // /etc/hosts: local name -> address mappings consulted before
                // DNS (so "localhost" resolves without a query).
                writeSystemFile("/etc/hosts",
                    "127.0.0.1\tlocalhost\n"
                    "10.0.2.15\tinstantos\n");
                // Seed /etc/mtab from the live VFS mount table so df (no args)
                // and mount can enumerate filesystems. Each line has the six
                // standard fields: fsname dir type opts freq passno.
                {
                    char mtab[1024];
                    size_t off = 0;
                    auto appendStr = [&](const char* s) {
                        for (const char* p = s; *p && off + 1 < sizeof(mtab); ++p) {
                            mtab[off++] = *p;
                        }
                    };
                    VFS::get().forEachMount([&](const char* path, const char* type) {
                        appendStr(type);   // mnt_fsname (device) - use fs type name
                        appendStr(" ");
                        appendStr(path);   // mnt_dir
                        appendStr(" ");
                        appendStr(type);   // mnt_type
                        appendStr(" rw 0 0\n");  // opts freq passno
                    });
                    mtab[off] = '\0';
                    writeSystemFile("/etc/mtab", mtab);
                }
            }
            Console::get().drawText("RamFS at /etc: [ ");
            Console::get().setTextColor(etcMountResult == 0 ? 0x49ceee : 0xFF0000);
            Console::get().drawText(etcMountResult == 0 ? "OK" : "FAIL");
            Console::get().setTextColor(0xFFFFFF);
            Console::get().drawText(" ]\n");

            // A writable /var (tmpfs) holding login records (/var/run/utmp).
            // Seeded with a BOOT_TIME record + a root USER_PROCESS record so
            // uptime shows the real "up" duration and who/users list a session.
            static RamFS varfs;
            int varMountResult = VFS::get().mount(&varfs, "/var");
            if (varMountResult == 0) {
                VFS::get().mkdir("/var/run", 0755);
                VFS::get().mkdir("/var/log", 0755);
                uint64_t bootUnix = time_get_boot_unix();
                writeUtmpRecord(2 /*BOOT_TIME*/, "reboot", "~", bootUnix);
                writeUtmpRecord(7 /*USER_PROCESS*/, "root", "console", time_get_unix());
            }
            Console::get().drawText("RamFS at /var: [ ");
            Console::get().setTextColor(varMountResult == 0 ? 0x49ceee : 0xFF0000);
            Console::get().drawText(varMountResult == 0 ? "OK" : "FAIL");
            Console::get().setTextColor(0xFFFFFF);
            Console::get().drawText(" ]\n");



            if (libMountResult == 0) {
                Debug::initializeKernelSymbols();
                // graphics-compositor takes over the framebuffer, so login starts it after authentication.
                launchInitrdService("/bin/input-manager");
                launchInitrdService("/bin/storage-manager");
                launchInitrdService("/bin/process-manager");
                launchInitrdService("/bin/network-manager");
                launchInitrdService("/bin/font-manager");
                launchInitrdService("/bin/session-manager");
                launchInitrdService("/bin/login");
            }
        }
    } else {
        Console::get().drawText("No initrd found\n");
    }
#ifdef INSTANTOS_BOOT_SPINNER
    bootSpinner.finish();
    activeBootSpinner = nullptr;
#endif

    asm volatile("sti");

    Scheduler::get().yield(); // Start running processes
    while (true) {
        asm volatile("hlt");
    }
    
    // Note: Destructors would be called here if the kernel ever exits
    // call_destructors();
}
