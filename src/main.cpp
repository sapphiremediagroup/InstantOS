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
#include <drivers/usb/ohci.hpp>
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
    // USBInput::get().initialize();

    VirtIOGPUDriver& virtioGpu = VirtIOGPUDriver::get();
    if (virtioGpu.initialize()) {
        uint32_t gpuWidth = 0;
        uint32_t gpuHeight = 0;
        virtioGpu.getMode(&gpuWidth, &gpuHeight);
        fb.switchToVirtIO(virtioGpu.getFramebuffer(), gpuWidth, gpuHeight, virtioGpu.getPitch());
        Console::get().setVirtIO(true);
        fb.clear(0);
        virtioGpu.flush(0, 0, gpuWidth, gpuHeight);
        Console::get().drawText("[BOOT] VirtIO GPU: [ OK ]\n");
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
    
    Console::get().drawText("testing new\n");
    volatile auto* idk = new int[16];
    delete[] idk;
    Console::get().drawText("Hello World\n");
    
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
