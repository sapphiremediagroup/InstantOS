#pragma once

#include <stddef.h>
#include <stdint.h>
#include <idrv/idrv.h>

// Loads .idrv driver modules (include/idrv/idrv.h, docs/idrv.md): checks the
// file, copies and relocates the image (identity-mapped frames above the low
// window non-PIE programs map over), calls the module's init and its probe for
// each PCI device its table matches. A module that has run is never freed, as
// it may have left callbacks with the kernel (unloading is not done yet).
class ModuleLoader {
public:
    struct Loaded {
        char name[32];
        char version[16];
        char description[80];
        char source[96];
        uint64_t base;
        uint32_t imageSize;
        uint32_t relocations;
        uint32_t devices;    // probes that took a device
        bool initFailed;
        bool atBoot;
        const IdrvModule* module;
    };

    static constexpr size_t MAX_MODULES = 32;

    // Every *.idrv in `directory` whose table matches a device present. At
    // boot the root disk's /drivers goes first, then the initrd's /lib/drivers:
    // a module whose name is already loaded is skipped, so one on the disk
    // replaces the built-in one (a driver update without a new initrd).
    static void loadBootModules(const char* directory);

    // One module; requireDevice drops it again when no device matches (before
    // its init runs). Devices another module took are not offered to it.
    // Returns IDRV_OK or a negative IDRV_E* code (IDRV_EEXIST: a module of
    // that name is loaded; IDRV_ENOENT: no such file).
    static int loadFile(const char* path, bool requireDevice, bool atBoot);
    static int loadFromMemory(const void* data, size_t size, const char* source, bool requireDevice,
                              bool atBoot);

    static size_t count();
    static const Loaded* at(size_t index);

    static const IdrvKernelApi& api();
};
