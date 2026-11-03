#include <cpu/syscall/syscall.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/user/session.hpp>
#include <cpu/user/user.hpp>
#include <interrupts/timer.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <memory/heap.hpp>
#include <graphics/console.hpp>
#include <graphics/gpu.hpp>
#include <graphics/virtio_gpu.hpp>
#include <graphics/intel_gen9.hpp>
#include <graphics/venus.hpp>
#include <common/string.hpp>
#include <common/ports.hpp>
#include <common/krandom.hpp>
#include <time/time.hpp>
#include <time/tsc_timer.hpp>
#include <fs/storage/storage.hpp>
#include <fs/storage/storage_lock.hpp>
#include <fs/ahci/ahci.hpp>
#include <cpu/acpi/acpi.hpp>
#include <cpuid.h>

size_t strncpyToUser(char* user_dest, const char* kernel_src, size_t max_len) {
    if (max_len == 0) return 0;

    while (*kernel_src && (*kernel_src == ' ' || *kernel_src == '\t')) {
        kernel_src++;
    }

    size_t i = 0;
    size_t src_len = 0;
    
    const char* src_start = kernel_src;
    const char* src_end = kernel_src;
    while (*src_end) src_end++;
    
    while (src_end > src_start && (src_end[-1] == ' ' || src_end[-1] == '\t')) {
        src_end--;
    }
    
    src_len = src_end - src_start;
    
    char temp[256];
    if (max_len > sizeof(temp)) {
        max_len = sizeof(temp);
    }

    for (i = 0; i < max_len - 1 && i < src_len; i++) {
        temp[i] = src_start[i];
    }
    
    temp[i] = '\0';
    return Syscall::copyToUser(reinterpret_cast<uint64_t>(user_dest), temp, i + 1) ? i : (size_t)-1;
}

size_t uitoa(uint64_t value, char* buffer, size_t buffer_size) {
    if (buffer_size == 0) return 0;

    char temp[20];
    size_t i = 0;

    if (value == 0) {
        if (buffer_size > 1) {
            buffer[0] = '0';
            buffer[1] = '\0';
            return 1;
        } else {
            buffer[0] = '\0';
            return 0;
        }
    }

    while (value && i < sizeof(temp)) {
        temp[i++] = '0' + (value % 10);
        value /= 10;
    }

    size_t j = 0;
    while (i > 0 && j < buffer_size - 1) {
        buffer[j++] = temp[--i];
    }

    buffer[j] = '\0';
    return j;
}

namespace {
constexpr uint64_t USER_FB_BASE = 0x0000700000000000ULL;

bool currentProcessOwnsFramebuffer() {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return false;
    }

    // Only the graphics-compositor draws to the device; everything else,
    // NetSurf included, is a window on a compositor surface.
    const char* name = current->getName();
    return strcmp(name, "/bin/graphics-compositor") == 0;
}

bool mapFramebufferIntoCurrentProcess(iFramebuffer* fb, uint64_t* userBase) {
    if (!fb || !userBase) {
        return false;
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return false;
    }

    const uint64_t fbPhys = reinterpret_cast<uint64_t>(fb->getRaw());
    const size_t fbSizeBytes = fb->getFBSize();
    const size_t pages = (fbSizeBytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t flags = Present | ReadWrite | UserSuper | NoExecute | kSharedFrame;
    // A device frame buffer (the firmware's, in the GPU aperture on a real PC)
    // is written by the CPU only: write-combining. virtio-gpu's is RAM.
    VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();
    if (!gpu.isInitialized() || gpu.getFramebuffer() != fb->getRaw()) {
        flags |= WriteCombine;
    }

    VMM::MapRangeInto(current->getPageTable(), USER_FB_BASE, fbPhys, pages, flags);
    *userBase = USER_FB_BASE;
    // The screen is the compositor's now: kernel messages go to the log only
    // (dmesg, /kernel.log), not over the desktop. Panics turn it back on.
    Console::get().setFramebufferOutputEnabled(false);
    return true;
}
}

uint64_t Syscall::sys_osinfo(uint64_t info_ptr) {
    if (!isValidUserPointer(info_ptr, sizeof(OSInfo))) {
        return (uint64_t)-1;
    }
    
    OSInfo info;

    memset(&info, 0, sizeof(OSInfo));

    unsigned int eax, ebx, ecx, edx;
    char vendor[13];
    __get_cpuid(0, &eax, &ebx, &ecx, &edx);

    *((unsigned int*)&vendor[0]) = ebx;
    *((unsigned int*)&vendor[4]) = edx;
    *((unsigned int*)&vendor[8]) = ecx;
    vendor[12] = '\0';

    char brand[49];
    memset(brand, 0, sizeof(brand));    
    __get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004) {
        char* brand_ptr = brand;
        for (unsigned int i = 0; i < 3; i++) {
            unsigned int regs[4];
            __get_cpuid(0x80000002 + i, &regs[0], &regs[1], &regs[2], &regs[3]);
            memcpy(brand_ptr + i*16, regs, 16);
        }
        brand[48] = '\0';
    } else {
        strncpy(brand, vendor, sizeof(brand)-1);
        brand[sizeof(brand)-1] = '\0';
    }

    strncpy(info.osname, "InstantOS", sizeof(info.osname) - 1);

    const char* loggedOnUser = "root";
    Process* current = Scheduler::get().getCurrentProcess();
    if (current) {
        Session* session = SessionManager::get().getSessionByPID(current->getPID());
        if (session) {
            User* user = UserManager::get().getUserByUID(session->uid);
            if (user) loggedOnUser = user->username;
        }
    }
    strncpy(info.loggedOnUser, loggedOnUser, sizeof(info.loggedOnUser) - 1);

    strncpy(info.cpuname, brand, sizeof(info.cpuname) - 1);

    char buf[16];
    memset(buf, 0, sizeof(buf));
    uitoa(PMM::UsableMemory() / (1024 * 1024), buf, sizeof(buf));
    strncpy(info.maxRamGB, buf, sizeof(info.maxRamGB) - 1);
    
    uint64_t usedBytes = PMM::UsedUsableMemory();
    uint64_t usedMB = usedBytes / (1024 * 1024);
    char buf2[16];
    memset(buf2, 0, sizeof(buf2));
    uitoa(usedMB, buf2, sizeof(buf2));
    strncpy(info.usedRamGB, buf2, sizeof(info.usedRamGB) - 1);

    info.major   = 0;
    info.minor   = 0;
    info.patch   = 0;
    info.buildnum = 0;

    return copyToUser(info_ptr, &info, sizeof(OSInfo)) ? 0 : (uint64_t)-1;
}

uint64_t Syscall::sys_storage_info(uint64_t infoPtr) {
    if (!isValidUserPointer(infoPtr, sizeof(StorageInfo))) {
        return syscall_error(SysErrInvalid);
    }

    StorageInfo info = KernelStorage::snapshot();
    return copyToUser(infoPtr, &info, sizeof(info)) ? 0 : syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_storage_format() {
    return KernelStorage::formatFat32() == 0 ? 0 : syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_storage_mount() {
    return KernelStorage::mountFat32() == 0 ? 0 : syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_gettime() {
    return Timer::get().getMilliseconds();
}

// Monotonic nanoseconds since boot (CLOCK_MONOTONIC in mlibc).
uint64_t Syscall::sys_gettime_ns() {
    return time_get_uptime_ns();
}

uint64_t Syscall::sys_getunixtime() {
    return time_get_unix();
}

uint64_t Syscall::sys_get_unix_time_ns() {
    return time_get_unix_ns();
}

// dmesg: the newest `length` bytes the kernel logged, oldest first.
uint64_t Syscall::sys_kernel_log(uint64_t buffer, uint64_t length) {
    if (buffer == 0 || length == 0) {
        return syscall_error(SysErrInvalid);
    }
    if (length > 128 * 1024) {
        length = 128 * 1024;
    }
    char* copy = static_cast<char*>(kmalloc(length));
    if (!copy) {
        return syscall_error(SysErrNoMemory);
    }
    const size_t count = Cereal::get().copyLog(copy, length);
    const bool copied = copyToUser(buffer, copy, count);
    kfree(copy);
    return copied ? count : syscall_error(SysErrInvalid);
}

// Restarts or powers off the machine; root only. The drives' write caches are
// flushed first (the FAT32 driver writes through, but the drive may still hold
// the data), so the last kernel log lines and files reach the disk.
uint64_t Syscall::sys_reboot(uint64_t mode) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || !current->isPrivileged()) {
        return syscall_error(SysErrPermission);
    }
    const auto how = static_cast<RebootMode>(mode);
    if (how != RebootMode::Restart && how != RebootMode::PowerOff) {
        return syscall_error(SysErrInvalid);
    }
    Cereal::get().write(how == RebootMode::Restart ? "[power] restarting\n" : "[power] powering off\n");
    {
        StorageLock::Section storage;
        if (!AHCIPort::flushAll()) {
            Cereal::get().write("[power] a disk flush failed\n");
        }
    }
    if (how == RebootMode::PowerOff) {
        ACPI::get().sysShutdown();
        Cereal::get().write("[power] power-off did not take; restarting instead\n");
    }
    ACPI::get().reboot();
    return 0;
}

// settimeofday/clock_settime(CLOCK_REALTIME); root only, like Linux.
uint64_t Syscall::sys_set_unix_time(uint64_t unixNs) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || !current->isPrivileged()) {
        return syscall_error(SysErrPermission);
    }
    if (static_cast<int64_t>(unixNs) < 0 || !time_set_unix_ns(unixNs)) {
        return syscall_error(SysErrInvalid);
    }
    Cereal::get().write("[time] wall clock set\n");
    return 0;
}

uint64_t Syscall::sys_get_entropy(uint64_t buffer, uint64_t length) {
    // getentropy(2): at most 256 bytes per call.
    if (length == 0) return 0;
    if (length > 256) return syscall_error(SysErrInvalid);
    if (!isValidUserPointer(buffer, length)) return syscall_error(SysErrInvalid);

    uint8_t local[256];
    kernel_fill_entropy(local, length);
    return copyToUser(buffer, local, length) ? 0 : syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_clear() {
    Console::get().drawText("\033[2J");
    return 0;
}

struct FBInfoStruct {
    uint64_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t pixelFormat;
    uint64_t fbSize;
    uint32_t redMask;
    uint32_t greenMask;
    uint32_t blueMask;
};

uint64_t Syscall::sys_fb_info(uint64_t info_ptr) {
    iFramebuffer* fb = Console::get().getFramebuffer();
    if (!fb) return (uint64_t)-1;

    if (!isValidUserPointer(info_ptr, sizeof(FBInfoStruct))) {
        return (uint64_t)-1;
    }
    // Only the compositor gets the framebuffer mapped; anyone may read the
    // screen geometry (apps size their windows by it), with addr = 0.
    uint64_t userBase = 0;
    if (currentProcessOwnsFramebuffer() && !mapFramebufferIntoCurrentProcess(fb, &userBase)) {
        return (uint64_t)-1;
    }

    FBInfoStruct kernel_info;
    kernel_info.addr       = userBase;
    kernel_info.width      = fb->getWidth();
    kernel_info.height     = fb->getHeight();
    kernel_info.pitch      = fb->getPitch();
    kernel_info.pixelFormat = static_cast<uint32_t>(fb->getPixelFormat());
    kernel_info.fbSize     = fb->getFBSize();
    kernel_info.redMask    = fb->getRedMaskSize();
    kernel_info.greenMask  = fb->getGreenMaskSize();
    kernel_info.blueMask   = fb->getBlueMaskSize();

    return copyToUser(info_ptr, &kernel_info, sizeof(FBInfoStruct)) ? 0 : (uint64_t)-1;
}

uint64_t Syscall::sys_fb_map() {
    iFramebuffer* fb = Console::get().getFramebuffer();
    if (!fb) return (uint64_t)-1;
    if (!currentProcessOwnsFramebuffer()) return (uint64_t)-1;

    uint64_t userBase = 0;
    if (!mapFramebufferIntoCurrentProcess(fb, &userBase)) {
        return (uint64_t)-1;
    }

    return userBase;
}

// The compositor's scanout buffers (Display syscall), mapped above the
// framebuffer's own mapping, 256 MiB apart.
constexpr uint64_t USER_SCANOUT_BASE = USER_FB_BASE + 0x10000000ULL;
constexpr uint64_t USER_SCANOUT_STRIDE = 0x10000000ULL;
static_assert(intel_gen9::IntelGen9Driver::kScanoutBuffers == kDisplayMaxBuffers);

namespace {

// Without the Intel display the buffers are RAM pages and a flip copies one
// to the framebuffer (and flushes it, for virtio-gpu): the compositor's
// double buffering can be watched in QEMU. It never runs unless the
// compositor is told to use flips (/display.cfg).
struct EmulatedDisplay {
    uint64_t* frames[kDisplayMaxBuffers];
    uint64_t pageCount;
};
EmulatedDisplay s_emulated = {};

uint64_t emulatedDisplay(iFramebuffer* fb, Process* current, DisplayCommand command, uint64_t arg) {
    const uint64_t pitchBytes = fb->getPitch() * 4;
    const uint64_t bytes = pitchBytes * fb->getHeight();
    switch (command) {
        case DisplayCommand::Acquire: {
            if (!Syscall::isValidUserPointer(arg, sizeof(DisplayBuffersInfo))) {
                return syscall_error(SysErrInvalid);
            }
            // Only a caller that asks for emulation (flags = kDisplayEmulated) gets it.
            DisplayBuffersInfo info = {};
            if (!Syscall::copyFromUser(&info, arg, sizeof(info)) || (info.flags & kDisplayEmulated) == 0) {
                return syscall_error(SysErrNoDevice);
            }
            info = {};
            const uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
            if (pages * PAGE_SIZE > USER_SCANOUT_STRIDE) {
                return syscall_error(SysErrNoDevice);
            }
            for (uint32_t i = 0; i < kDisplayMaxBuffers; ++i) {
                if (!s_emulated.frames[i]) {
                    auto* frames = static_cast<uint64_t*>(kmalloc(pages * sizeof(uint64_t)));
                    if (!frames) {
                        return syscall_error(SysErrNoMemory);
                    }
                    for (uint64_t page = 0; page < pages; ++page) {
                        frames[page] = PMM::AllocFrame();
                        if (frames[page] == 0) {
                            return syscall_error(SysErrNoMemory);   // leaks; test-only path
                        }
                    }
                    s_emulated.frames[i] = frames;
                    s_emulated.pageCount = pages;
                }
                const uint64_t base = USER_SCANOUT_BASE + i * USER_SCANOUT_STRIDE;
                for (uint64_t page = 0; page < s_emulated.pageCount; ++page) {
                    VMM::MapPageInto(current->getPageTable(), base + page * PAGE_SIZE, s_emulated.frames[i][page],
                                     Present | ReadWrite | UserSuper | NoExecute | kSharedFrame);
                }
                info.address[i] = base;
            }
            info.count = kDisplayMaxBuffers;
            info.width = fb->getWidth();
            info.height = fb->getHeight();
            info.pitch = static_cast<uint32_t>(pitchBytes);
            info.flags = kDisplayEmulated;
            return Syscall::copyToUser(arg, &info, sizeof(info)) ? 0 : syscall_error(SysErrInvalid);
        }
        case DisplayCommand::Flip: {
            if (arg >= kDisplayMaxBuffers || !s_emulated.frames[arg]) {
                return syscall_error(SysErrInvalid);
            }
            auto* screen = reinterpret_cast<uint8_t*>(fb->getRaw());
            for (uint64_t page = 0; page < s_emulated.pageCount; ++page) {
                const uint64_t offset = page * PAGE_SIZE;
                const uint64_t count = bytes - offset < PAGE_SIZE ? bytes - offset : PAGE_SIZE;
                memcpy(screen + offset, reinterpret_cast<const void*>(VMM::PhysToVirt(s_emulated.frames[arg][page])),
                       count);
            }
            GPU& gpu = GPU::get();
            if (gpu.isInitialized()) {
                gpu.flush(0, 0, fb->getWidth(), fb->getHeight());
            }
            return 0;
        }
        case DisplayCommand::Wait:
        case DisplayCommand::Release:
            return 0;
        case DisplayCommand::ExportBuffer:
            return syscall_error(SysErrNoDevice);   // the emulated buffers are not GPU memory
    }
    return syscall_error(SysErrInvalid);
}

} // namespace

uint64_t Syscall::sys_display(uint64_t command, uint64_t arg) {
    if (!currentProcessOwnsFramebuffer()) {
        return syscall_error(SysErrPermission);
    }
    Process* current = Scheduler::get().getCurrentProcess();
    intel_gen9::IntelGen9Driver& gpu = intel_gen9::IntelGen9Driver::get();
    if (!gpu.isInitialized()) {
        iFramebuffer* fb = Console::get().getFramebuffer();
        if (!fb || !current) {
            return syscall_error(SysErrNoDevice);
        }
        return emulatedDisplay(fb, current, static_cast<DisplayCommand>(command), arg);
    }
    switch (static_cast<DisplayCommand>(command)) {
        case DisplayCommand::Acquire: {
            if (!isValidUserPointer(arg, sizeof(DisplayBuffersInfo))) {
                return syscall_error(SysErrInvalid);
            }
            DisplayBuffersInfo info = {};
            if (!gpu.displayAcquire(current, USER_SCANOUT_BASE, USER_SCANOUT_STRIDE, info.address)) {
                return syscall_error(SysErrNoDevice);
            }
            info.count = intel_gen9::IntelGen9Driver::kScanoutBuffers;
            info.width = gpu.mode().width;
            info.height = gpu.mode().height;
            info.pitch = gpu.mode().stride;
            return copyToUser(arg, &info, sizeof(info)) ? 0 : syscall_error(SysErrInvalid);
        }
        case DisplayCommand::Flip:
            return gpu.displayFlip(static_cast<uint32_t>(arg)) ? 0 : syscall_error(SysErrInvalid);
        case DisplayCommand::Wait:
            return gpu.displayWait(arg > 1000 ? 1000 : arg) ? 0 : syscall_error(SysErrTimedOut);
        case DisplayCommand::Release:
            gpu.displayRelease();
            return 0;
        case DisplayCommand::ExportBuffer: {
            const int64_t handle = gpu.displayExportBuffer(static_cast<uint32_t>(arg));
            return static_cast<uint64_t>(handle);   // a handle, or -errno
        }
    }
    return syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_fb_flush(uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!currentProcessOwnsFramebuffer()) return (uint64_t)-1;

    GPU& gpu = GPU::get();
    if (gpu.isInitialized()) {
        return gpu.flush(static_cast<uint32_t>(x),
                         static_cast<uint32_t>(y),
                         static_cast<uint32_t>(w),
                         static_cast<uint32_t>(h)) ? 0 : (uint64_t)-1;
    }

    return 0;
}

uint64_t Syscall::sys_gpu_capset_info(uint64_t infoPtr) {
    if (!isValidUserPointer(infoPtr, sizeof(GPUCapsetInfo))) {
        return static_cast<uint64_t>(-1);
    }

    GPUCapsetInfo query = {};
    if (!copyFromUser(&query, infoPtr, sizeof(query))) {
        return static_cast<uint64_t>(-1);
    }

    VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();
    VirtIOGPUCapsetInfo info = {};
    if (!gpu.getCapsetInfo(query.index, &info)) {
        return static_cast<uint64_t>(-1);
    }

    query.capsetId = info.capset_id;
    query.capsetMaxVersion = info.capset_max_version;
    query.capsetMaxSize = info.capset_max_size;
    return copyToUser(infoPtr, &query, sizeof(query)) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_gpu_capset(uint64_t dataPtr) {
    if (!isValidUserPointer(dataPtr, sizeof(GPUCapsetData))) {
        return static_cast<uint64_t>(-1);
    }

    GPUCapsetData request = {};
    if (!copyFromUser(&request, dataPtr, sizeof(request))) {
        return static_cast<uint64_t>(-1);
    }
    if (request.bufferSize == 0 || !isValidUserPointer(request.buffer, request.bufferSize)) {
        return static_cast<uint64_t>(-1);
    }

    void* kernelBuffer = kmalloc(request.bufferSize);
    if (!kernelBuffer) {
        return static_cast<uint64_t>(-1);
    }

    VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();
    uint32_t actualSize = 0;
    const bool ok = gpu.getCapset(request.capsetId, request.capsetVersion, kernelBuffer, request.bufferSize, &actualSize);
    if (ok) {
        request.actualSize = actualSize;
    }

    bool copied = false;
    if (ok) {
        copied = copyToUser(request.buffer, kernelBuffer, request.actualSize) &&
                 copyToUser(dataPtr, &request, sizeof(request));
    }
    kfree(kernelBuffer);
    return copied ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_gpu_context_create(uint64_t createPtr) {
    if (!isValidUserPointer(createPtr, sizeof(GPUContextCreate))) {
        return static_cast<uint64_t>(-1);
    }

    GPUContextCreate request = {};
    if (!copyFromUser(&request, createPtr, sizeof(request))) {
        return static_cast<uint64_t>(-1);
    }

    VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();
    uint32_t ctxId = 0;
    const bool ok = request.capsetId != 0
        ? gpu.createContextWithCapset(&ctxId, request.capsetId, request.debugName,
                                      request.contextInit, request.ringIdx, request.useRingIdx != 0)
        : GPU::get().createContext(&ctxId, request.debugName,
                                   request.contextInit, request.ringIdx, request.useRingIdx != 0);
    if (!ok) {
        return static_cast<uint64_t>(-1);
    }

    request.ctxId = ctxId;
    return copyToUser(createPtr, &request, sizeof(request)) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_gpu_context_destroy(uint64_t ctxId) {
    return GPU::get().destroyContext(static_cast<uint32_t>(ctxId)) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_gpu_resource_create_3d(uint64_t createPtr) {
    if (!isValidUserPointer(createPtr, sizeof(GPUResourceCreate3D))) {
        return static_cast<uint64_t>(-1);
    }

    GPUResourceCreate3D request = {};
    if (!copyFromUser(&request, createPtr, sizeof(request))) {
        return static_cast<uint64_t>(-1);
    }

    VirtIOGPUResourceCreate3D resource = {};
    resource.hdr.ctx_id = request.ctxId;
    resource.target = request.target;
    resource.format = request.format;
    resource.bind = request.bind;
    resource.width = request.width;
    resource.height = request.height;
    resource.depth = request.depth;
    resource.array_size = request.arraySize;
    resource.last_level = request.lastLevel;
    resource.nr_samples = request.nrSamples;
    resource.flags = request.flags;

    uint32_t resourceIdValue = 0;
    if (!VirtIOGPUDriver::get().createResource3D(resource, &resourceIdValue)) {
        return static_cast<uint64_t>(-1);
    }

    request.resourceId = resourceIdValue;
    return copyToUser(createPtr, &request, sizeof(request)) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_gpu_resource_destroy(uint64_t destroyPtr) {
    if (!isValidUserPointer(destroyPtr, sizeof(GPUResourceDestroy))) {
        return static_cast<uint64_t>(-1);
    }

    GPUResourceDestroy request = {};
    if (!copyFromUser(&request, destroyPtr, sizeof(request))) {
        return static_cast<uint64_t>(-1);
    }

    return VirtIOGPUDriver::get().destroyResource3D(request.ctxId, request.resourceId, request.hasBacking != 0)
        ? 0
        : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_gpu_resource_assign_uuid(uint64_t uuidPtr) {
    if (!isValidUserPointer(uuidPtr, sizeof(GPUResourceUUID))) {
        return static_cast<uint64_t>(-1);
    }

    GPUResourceUUID request = {};
    if (!copyFromUser(&request, uuidPtr, sizeof(request))) {
        return static_cast<uint64_t>(-1);
    }

    if (!VirtIOGPUDriver::get().assignResourceUUID(request.resourceId, request.uuid)) {
        return static_cast<uint64_t>(-1);
    }

    return copyToUser(uuidPtr, &request, sizeof(request)) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_gpu_submit_3d(uint64_t submitPtr) {
    if (!isValidUserPointer(submitPtr, sizeof(GPUSubmit3D))) {
        return static_cast<uint64_t>(-1);
    }

    GPUSubmit3D request = {};
    if (!copyFromUser(&request, submitPtr, sizeof(request))) {
        return static_cast<uint64_t>(-1);
    }
    if (request.size == 0 || !isValidUserPointer(request.commands, request.size)) {
        return static_cast<uint64_t>(-1);
    }

    void* kernelCommands = kmalloc(request.size);
    if (!kernelCommands) {
        return static_cast<uint64_t>(-1);
    }
    if (!copyFromUser(kernelCommands, request.commands, request.size)) {
        kfree(kernelCommands);
        return static_cast<uint64_t>(-1);
    }

    GPUCommand command = {};
    GPUCommandResult result = {};
    command.type = GPUCommandType::Submit3D;
    command.contextId = request.ctxId;
    command.data = kernelCommands;
    command.size = request.size;

    const bool ok = GPU::get().submitCommand(command, &result);
    kfree(kernelCommands);

    const VirtIOGPUCommandStatus status = VirtIOGPUDriver::get().getLastCommandStatus();
    request.transportOk = ok ? 1 : 0;
    request.responseOk = status.responseOk ? 1 : 0;
    request.responseType = result.status ? result.status : status.responseType;
    request.submittedFence = result.value64 ? result.value64 : status.submittedFence;
    request.completedFence = status.completedFence;

    return copyToUser(submitPtr, &request, sizeof(request)) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_gpu_wait_fence(uint64_t waitPtr) {
    if (!isValidUserPointer(waitPtr, sizeof(GPUWaitFence))) {
        return static_cast<uint64_t>(-1);
    }

    GPUWaitFence request = {};
    if (!copyFromUser(&request, waitPtr, sizeof(request))) {
        return static_cast<uint64_t>(-1);
    }

    const uint64_t spinLimit = request.timeoutIterations != 0 ? request.timeoutIterations : 1000000ULL;
    uint64_t completedFence = 0;
    uint32_t responseType = 0;
    const bool ok = GPU::get().waitForFence(request.fenceId, spinLimit, &completedFence, &responseType);

    request.completed = ok ? 1 : 0;
    request.completedFence = completedFence;
    request.responseType = responseType;
    return copyToUser(waitPtr, &request, sizeof(request)) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_gpu_venus_probe(uint64_t probePtr) {
    if (!isValidUserPointer(probePtr, sizeof(GPUVenusProbe))) {
        return static_cast<uint64_t>(-1);
    }

    GPUVenusProbe out = {};

    venus::Venus& vk = venus::Venus::get();
    out.available = vk.negotiate() ? 1 : 0;

    if (out.available) {
        venus::VenusProbeResult probe = {};
        const bool ok = vk.probe(&probe);
        out.replyOk = (ok && probe.replyOk) ? 1 : 0;
        out.capsetVersion = probe.venusProtocolVersion;
        out.wireFormatVersion = probe.wireFormatVersion;
        out.vkXmlVersion = probe.vkXmlVersion;
        out.instanceVersion = probe.instanceVersion;
        out.responseType = probe.responseType;
    }

    return copyToUser(probePtr, &out, sizeof(out)) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_gpu_venus_vulkan(uint64_t resultPtr) {
    if (!isValidUserPointer(resultPtr, sizeof(GPUVenusVulkan))) {
        return static_cast<uint64_t>(-1);
    }

    GPUVenusVulkan out = {};

    venus::Venus& vk = venus::Venus::get();
    if (vk.negotiate()) {
        venus::VenusVulkanResult vkr = {};
        vk.bringUpVulkan(&vkr);
        out.ringOk = vkr.ringOk ? 1 : 0;
        out.instanceOk = vkr.instanceOk ? 1 : 0;
        out.physDevOk = vkr.physDevOk ? 1 : 0;
        out.propsOk = vkr.propsOk ? 1 : 0;
        out.deviceOk = vkr.deviceOk ? 1 : 0;
        out.physDevCount = vkr.physDevCount;
        out.apiVersion = vkr.device0.apiVersion;
        out.driverVersion = vkr.device0.driverVersion;
        out.vendorId = vkr.device0.vendorId;
        out.deviceId = vkr.device0.deviceId;
        out.deviceType = vkr.device0.deviceType;
        out.computeOk = vkr.computeOk ? 1 : 0;
        out.computeElements = vkr.computeElements;
        out.computeMismatches = vkr.computeMismatches;
        out.instanceHandle = vkr.instanceHandle;
        out.deviceHandle = vkr.deviceHandle;
        // Copy the device name safely (bounded).
        for (uint32_t i = 0; i < sizeof(out.deviceName) - 1; ++i) {
            out.deviceName[i] = vkr.device0.deviceName[i];
            if (vkr.device0.deviceName[i] == '\0') {
                break;
            }
        }
        out.deviceName[sizeof(out.deviceName) - 1] = '\0';
    }

    return copyToUser(resultPtr, &out, sizeof(out)) ? 0 : static_cast<uint64_t>(-1);
}

extern "C" void _purecall() {
    while (1);
}
