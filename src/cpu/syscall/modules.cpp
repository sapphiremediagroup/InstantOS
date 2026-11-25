// ModuleList / ModuleLoad: the .idrv driver modules (docs/idrv.md).
#include <cpu/syscall/syscall.hpp>

#include <common/string.hpp>
#include <cpu/process/process.hpp>
#include <cpu/process/scheduler.hpp>
#include <drivers/modules/module_loader.hpp>

namespace {
void copyText(char* out, size_t size, const char* text) {
    size_t i = 0;
    for (; text && text[i] && i + 1 < size; ++i) {
        out[i] = text[i];
    }
    out[i] = '\0';
}
}  // namespace

uint64_t Syscall::sys_module_list(uint64_t buffer, uint64_t capacity) {
    const size_t count = ModuleLoader::count();
    for (size_t i = 0; buffer != 0 && i < count && i < capacity; ++i) {
        const ModuleLoader::Loaded* m = ModuleLoader::at(i);
        ModuleInfo info = {};
        copyText(info.name, sizeof(info.name), m->name);
        copyText(info.version, sizeof(info.version), m->version);
        copyText(info.description, sizeof(info.description), m->description);
        copyText(info.source, sizeof(info.source), m->source);
        info.base = m->base;
        info.imageSize = m->imageSize;
        info.devices = m->devices;
        info.relocations = m->relocations;
        info.flags = (m->initFailed ? ModuleInitFailed : 0u) | (m->atBoot ? ModuleAtBoot : 0u);
        if (!copyToUser(buffer + i * sizeof(ModuleInfo), &info, sizeof(info))) {
            return syscall_error(SysErrInvalid);
        }
    }
    return count;
}

uint64_t Syscall::sys_module_load(uint64_t path) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || !current->isPrivileged()) {
        return syscall_error(SysErrPermission);
    }
    char file[256];
    if (path == 0 || !copyStringFromUser(path, file, sizeof(file)) || file[0] != '/') {
        return syscall_error(SysErrInvalid);   // an absolute path (modload makes one)
    }
    switch (ModuleLoader::loadFile(file, true, false)) {
    case IDRV_OK: return 0;
    case IDRV_ENOENT: return syscall_error(SysErrNoEntry);
    case IDRV_EEXIST: return syscall_error(SysErrExists);
    case IDRV_ENODEV: return syscall_error(SysErrNoDevice);
    case IDRV_ENOMEM: return syscall_error(SysErrNoMemory);
    case IDRV_EIO: return syscall_error(SysErrIO);
    default: return syscall_error(SysErrInvalid);
    }
}
