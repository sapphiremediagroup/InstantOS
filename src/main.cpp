#include "Uefi/UefiBaseType.h"
#include "cpu/process/scheduler.hpp"
#include <cpu/process/exec.hpp>
#include "cpu/syscall/syscall.hpp"
#include "cpu/user/session.hpp"
#include "cpu/user/user.hpp"
#include "fs/ahci/ahci.hpp"
#include "fs/ahci/detect.hpp"
#include "fs/fat32/fat32.hpp"
#include "fs/ramfs/ramfs.hpp"
#include <fs/initrd/initrd.hpp>
#include <iboot/memory.hpp>
#include <cpu/gdt/gdt.hpp>
#include <cpu/idt/idt.hpp>
#include <cpu/apic/irqs.hpp>
#include <memory/pmm.hpp>
#include <memory/heap.hpp>
#include <memory/vmm.hpp>
#include <graphics/framebuffer.hpp>
#include <graphics/console.hpp>
#include <cpu/acpi/acpi.hpp>
#include <cpu/apic/apic.hpp>
#include <cpu/apic/pic.hpp>
#include <cpu/cpuid.hpp>
#include <debug/diag.hpp>
#include <drivers/hid/i2c_hid.hpp>
#include <drivers/usb/ohci.hpp>
#include <drivers/usb/xhci.hpp>
#include <interrupts/keyboard.hpp>
#include <interrupts/timer.hpp>
#include <time/time.hpp>
#include <graphics/virtio_gpu.hpp>
unsigned long long runtimeBase;

namespace {
struct TableRegister {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

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
    logBootText("[input] no keyboard detected; waiting for PS/2, USB, or I2C keyboard input\n");
    Console::get().setTextColor(0xFFFFFF);
    drawKeyboardStatus("[input] state:");
    drawKeyboardFailureHints();

    uint32_t polls = 0;
    while (!hasAnyKeyboard()) {
        pollInputBackends();
        if (++polls == 2000000) {
            drawKeyboardStatus("[input] still waiting:");
            drawKeyboardFailureHints();
            polls = 0;
        }
        asm volatile("pause");
    }

    drawKeyboardStatus("[input] keyboard detected:");
}

void dumpVirtIOGPUCaps(VirtIOGPUDriver& virtioGpu) {
    drawBoolLine("[VGPU] virgl=", virtioGpu.supportsVirgl());
    drawBoolLine("[VGPU] context_init=", virtioGpu.supportsContextInit());
    drawBoolLine("[VGPU] resource_blob=", virtioGpu.supportsBlobResources());
    drawBoolLine("[VGPU] resource_uuid=", virtioGpu.supportsResourceUUID());
    drawBoolLine("[VGPU] scanout_blob_active=", virtioGpu.isUsingBlobScanout());

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

    GDT::get().initialize();
    
    if (!CPU::initialize()) {
        while (1) {
            asm("hlt");
        }
    }
    
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
    
    VMM::Initialize();
    
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

    if(!ACPI::get().initialize(bootInfo->rsdp)) {
        Console::get().drawText("ACPI: [ ");
        Console::get().setTextColor(0xFF0000);
        Console::get().drawText("FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
    }

    IDT::get();
    PIC::disable();

    if(!APICManager::get().initialize()) {
        Console::get().drawText("APIC: [ ");
        Console::get().setTextColor(0xFF0000);
        Console::get().drawText("FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
    }
    
    APICManager& apic = APICManager::get();
    uint8_t targetCore = static_cast<uint8_t>(LAPIC::get().getId());

    drawTextLine("[BOOT] before mapIRQ timer");
    apic.mapIRQ(IRQ_TIMER, VECTOR_TIMER, targetCore);
    drawTextLine("[BOOT] after mapIRQ timer");
    Scheduler::get().initialize();
    
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
    USBInput::get().initialize();
    I2CHIDController::get().initialize();

#ifndef INPUT_PROBE_ONLY
    waitForKeyboardBeforeBoot();
#endif

#ifdef INPUT_PROBE_ONLY
    runInputProbeOnly();
#endif

    VirtIOGPUDriver& virtioGpu = VirtIOGPUDriver::get();
    Console::get().drawText("[BOOT] framebuffer fallback=");
    Console::get().drawNumber(static_cast<int64_t>(fb.getWidth()));
    Console::get().drawText("x");
    Console::get().drawNumber(static_cast<int64_t>(fb.getHeight()));
    Console::get().drawText("\n");
    virtioGpu.setFallbackDisplayMode(static_cast<uint32_t>(fb.getWidth()),
                                     static_cast<uint32_t>(fb.getHeight()));
    if (virtioGpu.initialize()) {
        uint32_t gpuWidth = 0;
        uint32_t gpuHeight = 0;
        virtioGpu.getMode(&gpuWidth, &gpuHeight);
        fb.switchToVirtIO(virtioGpu.getFramebuffer(), gpuWidth, gpuHeight, virtioGpu.getPitch());
        Console::get().setVirtIO(true);
        Console::get().setCopyScrollEnabled(true);
        fb.clear(0);
        virtioGpu.flush(0, 0, gpuWidth, gpuHeight);
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
        dumpVirtIOGPUFences(virtioGpu);
    } else {
        Console::get().drawText("[BOOT] VirtIO GPU: unavailable, using boot framebuffer\n");
    }
    
    Syscall::get().initialize();

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

    SATABlockDevice* device = nullptr;
    FAT32FS* fs = nullptr;
    Console::get().drawText("\033[2Jdetecting ahci\n");
    auto controller = AHCIDetector::detectAndInitialize();
    if (controller) {
        auto port = controller->getPort(0);
        if (port && port->isActive()) {
            static SATABlockDevice _device(port);
            device = &_device;
            static FAT32FS _fs(device);
            fs = &_fs;
            int mountResult = VFS::get().mount(fs, "/");
            if(mountResult != 0){
                Console::get().drawText("Filesystem mounted: [ ");
                Console::get().setTextColor(0xFF0000);
                Console::get().drawText("FAIL");
                Console::get().setTextColor(0xFFFFFF);
                Console::get().drawText(" ]\n");
            }
        } else {
            Console::get().drawText("AHCI Port: [ ");
            Console::get().setTextColor(0xFF0000);
            Console::get().drawText("FAIL");
            Console::get().setTextColor(0xFFFFFF);
            Console::get().drawText(" ]\n");
        }
    } else {
        Console::get().drawText("AHCI: [ ");
        Console::get().setTextColor(0xFF0000);
        Console::get().drawText("FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
    }
    
    
    if (bootInfo->initrdBase && bootInfo->initrdSize) {
        Console::get().drawText("Mounting initrdfs...\n");

        static InitrdFS initrdfs(
            reinterpret_cast<void*>(bootInfo->initrdBase),
            bootInfo->initrdSize
        );
        int mountResult = VFS::get().mount(&initrdfs, "/bin");
        Console::get().drawText("InitrdFS at /bin: [ ");
        Console::get().setTextColor(mountResult == 0 ? 0x49ceee : 0xFF0000);
        Console::get().drawText(mountResult == 0 ? "OK" : "FAIL");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");

        if (mountResult == 0) {
            Debug::initializeKernelSymbols();
            // graphics-compositor takes over the framebuffer, so login starts it after authentication.
            launchInitrdService("/bin/input-manager.exe");
            launchInitrdService("/bin/storage-manager.exe");
            launchInitrdService("/bin/process-manager.exe");
            launchInitrdService("/bin/font-manager.exe");
            launchInitrdService("/bin/session-manager.exe");
            launchInitrdService("/bin/login.exe");
        }
    } else {
        Console::get().drawText("No initrd found\n");
    }

    asm volatile("sti");

    Scheduler::get().yield(); // Start running processes
    while (true) {
        asm volatile("hlt");
    }
    
    // Note: Destructors would be called here if the kernel ever exits
    // call_destructors();
}
