#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/user/session.hpp>
#include <cpu/user/user.hpp>
#include <interrupts/timer.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <memory/heap.hpp>
#include <graphics/console.hpp>
#include <graphics/virtio_gpu.hpp>
#include <common/string.hpp>
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

    return strcmp(current->getName(), "/bin/graphics-compositor.exe") == 0;
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

    uint64_t flags = Present | ReadWrite | UserSuper | NoExecute;
    VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();
    if (!gpu.isInitialized() || gpu.getFramebuffer() != fb->getRaw()) {
        flags |= CacheDisab;
    }

    VMM::MapRangeInto(current->getPageTable(), USER_FB_BASE, fbPhys, pages, flags);
    *userBase = USER_FB_BASE;
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
    uitoa(PMM::TotalMemory() / (1024 * 1024), buf, sizeof(buf));
    strncpy(info.maxRamGB, buf, sizeof(info.maxRamGB) - 1);
    
    uint64_t usedBytes = PMM::UsedMemory();
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

uint64_t Syscall::sys_gettime() {
    return Timer::get().getMilliseconds();
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
    if (!currentProcessOwnsFramebuffer()) return (uint64_t)-1;

    if (!isValidUserPointer(info_ptr, sizeof(FBInfoStruct))) {
        return (uint64_t)-1;
    }
    uint64_t userBase = 0;
    if (!mapFramebufferIntoCurrentProcess(fb, &userBase)) {
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

uint64_t Syscall::sys_fb_flush(uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!currentProcessOwnsFramebuffer()) return (uint64_t)-1;

    VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();
    if (gpu.isInitialized()) {
        return gpu.flush(
            static_cast<uint32_t>(x),
            static_cast<uint32_t>(y),
            static_cast<uint32_t>(w),
            static_cast<uint32_t>(h)
        ) ? 0 : (uint64_t)-1;
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
        : gpu.createContext(&ctxId, request.debugName, request.contextInit,
                            request.ringIdx, request.useRingIdx != 0);
    if (!ok) {
        return static_cast<uint64_t>(-1);
    }

    request.ctxId = ctxId;
    return copyToUser(createPtr, &request, sizeof(request)) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_gpu_context_destroy(uint64_t ctxId) {
    return VirtIOGPUDriver::get().destroyContext(static_cast<uint32_t>(ctxId)) ? 0 : static_cast<uint64_t>(-1);
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

    const bool ok = VirtIOGPUDriver::get().submit3D(request.ctxId, kernelCommands, request.size);
    kfree(kernelCommands);

    const VirtIOGPUCommandStatus status = VirtIOGPUDriver::get().getLastCommandStatus();
    request.transportOk = ok ? 1 : 0;
    request.responseOk = status.responseOk ? 1 : 0;
    request.responseType = status.responseType;
    request.submittedFence = status.submittedFence;
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
    const bool ok = VirtIOGPUDriver::get().waitForFence(request.fenceId, spinLimit, &completedFence, &responseType);

    request.completed = ok ? 1 : 0;
    request.completedFence = completedFence;
    request.responseType = responseType;
    return copyToUser(waitPtr, &request, sizeof(request)) ? 0 : static_cast<uint64_t>(-1);
}

extern "C" void _purecall() {
    while (1);
}
