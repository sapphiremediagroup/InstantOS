#include <drivers/modules/module_loader.hpp>

#include <common/krandom.hpp>
#include <common/string.hpp>
#include <cpu/acpi/pci.hpp>
#include <cpu/acpi/pci_bus.hpp>
#include <cpu/apic/lapic.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/smp.hpp>
#include <drivers/net/net_device.hpp>
#include <fs/vfs/vfs.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <time/tsc_timer.hpp>

namespace {

ModuleLoader::Loaded gModules[ModuleLoader::MAX_MODULES];
size_t gModuleCount = 0;

// PCI functions a module's probe took: never offered to another module.
struct ClaimedDevice {
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};
constexpr size_t kMaxClaimed = 64;
ClaimedDevice gClaimed[kMaxClaimed];
size_t gClaimedCount = 0;

bool claimed(const PciDeviceInfo& d) {
    for (size_t i = 0; i < gClaimedCount; ++i) {
        const ClaimedDevice& c = gClaimed[i];
        if (c.segment == d.segment && c.bus == d.bus && c.device == d.device && c.function == d.function) {
            return true;
        }
    }
    return false;
}

void claim(const PciDeviceInfo& d) {
    if (gClaimedCount < kMaxClaimed) {
        gClaimed[gClaimedCount++] = {d.segment, d.bus, d.device, d.function};
    }
}

void apiLog(const char* text) {
    Cereal::get().write(text);
}

void logDec(uint64_t value) {
    char text[21];
    int i = 20;
    text[i] = '\0';
    do {
        text[--i] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    Cereal::get().write(&text[i]);
}

void logHex(uint64_t value) {
    char text[19] = "0x";
    int digits = 1;
    for (uint64_t v = value >> 4; v != 0; v >>= 4) {
        ++digits;
    }
    for (int i = 0; i < digits; ++i) {
        text[2 + digits - 1 - i] = "0123456789abcdef"[(value >> (i * 4)) & 0xF];
    }
    text[2 + digits] = '\0';
    Cereal::get().write(text);
}

uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// ---- The kernel API ------------------------------------------------------

uint64_t apiUptimeNs() {
    return time_get_uptime_ns();
}

// Not by the uptime: on one CPU, with interrupts off and no TSC clock (WHPX),
// it stands still.
void apiDelayUs(uint64_t microseconds) {
    SMP::delayUs(microseconds);
}

void* apiAlloc(size_t bytes) {
    return kmalloc(bytes);
}

void apiFree(void* pointer) {
    kfree(pointer);
}

uint64_t apiDmaAlloc(uint64_t pages, uint64_t maxPhys, void** virt) {
    if (pages == 0) {
        return 0;
    }
    uint64_t phys = 0;
    if (maxPhys != 0) {
        phys = PMM::AllocFramesBelow(pages, maxPhys);
    } else {
        phys = PMM::AllocFramesAbove(pages, PMM::KERNEL_HIGH_ALLOC_MIN);
        if (phys == 0) {
            phys = PMM::AllocFrames(pages);
        }
    }
    if (phys == 0) {
        return 0;
    }
    void* mapped = reinterpret_cast<void*>(VMM::PhysToVirt(phys));
    memset(mapped, 0, pages * PMM::PAGE_SIZE);
    if (virt) {
        *virt = mapped;
    }
    return phys;
}

void apiDmaFree(uint64_t phys, uint64_t pages) {
    if (phys != 0 && pages != 0) {
        PMM::FreeFrames(phys, pages);
    }
}

uint32_t apiPciRead(const IdrvPciDevice* d, uint16_t offset, uint32_t width) {
    PCI& pci = PCI::get();
    switch (width) {
    case 1: return pci.readConfig8(d->segment, d->bus, d->device, d->function, offset);
    case 2: return pci.readConfig16(d->segment, d->bus, d->device, d->function, offset);
    default: return pci.readConfig32(d->segment, d->bus, d->device, d->function, offset);
    }
}

void apiPciWrite(const IdrvPciDevice* d, uint16_t offset, uint32_t width, uint32_t value) {
    PCI& pci = PCI::get();
    switch (width) {
    case 1: pci.writeConfig8(d->segment, d->bus, d->device, d->function, offset, static_cast<uint8_t>(value)); break;
    case 2: pci.writeConfig16(d->segment, d->bus, d->device, d->function, offset, static_cast<uint16_t>(value)); break;
    default: pci.writeConfig32(d->segment, d->bus, d->device, d->function, offset, value); break;
    }
}

uint64_t apiPciMapBar(const IdrvPciDevice* d, uint32_t bar, uint64_t bytes) {
    return PCI::get().mapMemoryBar(d->segment, d->bus, d->device, d->function, static_cast<uint8_t>(bar), bytes);
}

// A module's interrupt, as the kernel's Interrupt. MSI vectors are the
// module's alone and EOId here; on a shared INTx line the dispatcher EOIs
// after every handler (and asks shouldDispatch first).
class ModuleInterrupt : public Interrupt {
public:
    void initialize() override {}

    bool shouldDispatch() override {
        if (msi) {
            return true;
        }
        return ops->claim && ops->claim(context) != 0;
    }

    void Run(InterruptFrame* frame) override {
        if (msi) {
            if (ops->claim) {
                (void)ops->claim(context);
            }
            sendEOI();
        }
        const uint32_t requests = ops->handle ? ops->handle(context) : 0;
        if (requests & IDRV_IRQ_NET_RECEIVE) {
            netDeviceInterrupt(frame, msi);
        }
    }

    const IdrvIrqOps* ops = nullptr;
    void* context = nullptr;
    bool msi = false;
};

constexpr size_t kMaxModuleInterrupts = 16;

ModuleInterrupt* moduleInterrupts() {
    static ModuleInterrupt pool[kMaxModuleInterrupts];   // constructed on first use
    return pool;
}
size_t gModuleInterruptCount = 0;

uint32_t apiIrqRegister(const IdrvPciDevice* d, uint32_t flags, const IdrvIrqOps* ops, void* context) {
    if (!ops || gModuleInterruptCount >= kMaxModuleInterrupts) {
        return IDRV_IRQ_NONE;
    }
    ModuleInterrupt& handler = moduleInterrupts()[gModuleInterruptCount];
    handler.ops = ops;
    handler.context = context;
    PCI& pci = PCI::get();
    if (flags & IDRV_IRQ_ALLOW_MSI) {
        handler.msi = true;
        if (pci.registerMSIInterrupt(d->segment, d->bus, d->device, d->function, &handler)) {
            ++gModuleInterruptCount;
            return IDRV_IRQ_MSI;
        }
    }
    if ((flags & IDRV_IRQ_ALLOW_LEGACY) && ops->claim) {
        handler.msi = false;
        if (pci.registerLegacyInterrupt(d->segment, d->bus, d->device, d->function, &handler)) {
            ++gModuleInterruptCount;
            return IDRV_IRQ_LEGACY;
        }
    }
    return IDRV_IRQ_NONE;
}

uint32_t apiIrqRegisterMsix(const IdrvPciDevice* d, uint32_t entry, const IdrvIrqOps* ops, void* context) {
    if (!ops || gModuleInterruptCount >= kMaxModuleInterrupts || entry > 2047) {
        return IDRV_IRQ_NONE;
    }
    ModuleInterrupt& handler = moduleInterrupts()[gModuleInterruptCount];
    handler.ops = ops;
    handler.context = context;
    handler.msi = true;   // a vector of its own: EOId here
    if (!PCI::get().registerMSIXInterrupt(d->segment, d->bus, d->device, d->function, static_cast<uint16_t>(entry),
                                          &handler)) {
        return IDRV_IRQ_NONE;
    }
    ++gModuleInterruptCount;
    return IDRV_IRQ_MSIX;
}

void apiRandom(void* buffer, size_t length) {
    if (buffer && length) {
        kernel_fill_entropy(buffer, length);
    }
}

// A module's network device, as the kernel's NetDevice.
class ModuleNetDevice : public NetDevice {
public:
    bool initialize() override { return ops != nullptr; }
    bool isInitialized() const override { return ops != nullptr; }
    bool isAvailable() const override { return ops != nullptr; }
    void getMacAddress(uint8_t* mac) override {
        if (mac) {
            ops->macAddress(context, mac);
        }
    }
    bool sendPacket(const void* data, size_t len) override {
        return data && len != 0 && len <= NET_DEVICE_MTU && ops->send(context, data, len) != 0;
    }
    int receivePacket(void* buffer, size_t maxLen) override {
        return buffer && maxLen ? ops->receive(context, buffer, maxLen) : -1;
    }
    bool isLinkUp() const override { return ops->linkUp(context) != 0; }
    bool interruptDriven() const override { return ops->interruptDriven && ops->interruptDriven(context) != 0; }
    const char* name() const override { return ops->name; }
    int probePriority() const override { return ops->probePriority; }

    const IdrvNetOps* ops = nullptr;
    void* context = nullptr;
};

constexpr size_t kMaxModuleNetDevices = 4;

ModuleNetDevice* moduleNetDevices() {
    static ModuleNetDevice pool[kMaxModuleNetDevices];
    return pool;
}
size_t gModuleNetDeviceCount = 0;

int apiNetRegister(const IdrvNetOps* ops, void* context) {
    if (!ops || !ops->macAddress || !ops->send || !ops->receive || !ops->linkUp || !ops->name) {
        return IDRV_EINVAL;
    }
    if (gModuleNetDeviceCount >= kMaxModuleNetDevices) {
        return IDRV_ENOMEM;
    }
    ModuleNetDevice& device = moduleNetDevices()[gModuleNetDeviceCount++];
    device.ops = ops;
    device.context = context;
    NetDeviceRegistry::registerDevice(&device);
    return IDRV_OK;
}

void* apiMemcpy(void* dst, const void* src, size_t n) {
    return memcpy(dst, src, n);
}

void* apiMemset(void* dst, int value, size_t n) {
    return memset(dst, value, n);
}

const IdrvKernelApi kApi = {
    IDRV_API_VERSION,
    sizeof(IdrvKernelApi),
    apiLog,
    logHex,
    logDec,
    apiUptimeNs,
    apiDelayUs,
    apiAlloc,
    apiFree,
    apiDmaAlloc,
    apiDmaFree,
    apiPciRead,
    apiPciWrite,
    apiPciMapBar,
    apiIrqRegister,
    apiNetRegister,
    apiMemcpy,
    apiMemset,
    apiIrqRegisterMsix,
    apiRandom,
};

// ---- Loading -------------------------------------------------------------

bool matches(const IdrvPciMatch& m, const PciDeviceInfo& d) {
    if (m.vendorId != IDRV_PCI_ANY && m.vendorId != d.vendorId) {
        return false;
    }
    if (m.deviceId != IDRV_PCI_ANY && m.deviceId != d.deviceId) {
        return false;
    }
    const uint32_t cls = (static_cast<uint32_t>(d.classCode) << 16) | (static_cast<uint32_t>(d.subclass) << 8) | d.progIf;
    return (cls & m.classMask) == m.classValue;
}

bool moduleMatches(const IdrvModule& module, const PciDeviceInfo& d) {
    for (uint32_t i = 0; i < module.pciMatchCount; ++i) {
        if (matches(module.pciMatches[i], d)) {
            return true;
        }
    }
    return false;
}

IdrvPciDevice toIdrv(const PciDeviceInfo& d) {
    IdrvPciDevice out = {};
    out.segment = d.segment;
    out.bus = d.bus;
    out.device = d.device;
    out.function = d.function;
    out.classCode = d.classCode;
    out.subclass = d.subclass;
    out.progIf = d.progIf;
    out.vendorId = d.vendorId;
    out.deviceId = d.deviceId;
    out.revision = d.revision;
    out.interruptPin = d.interruptPin;
    return out;
}

void copyName(char* out, size_t size, const char* text) {
    size_t i = 0;
    for (; text && text[i] && i + 1 < size; ++i) {
        out[i] = text[i];
    }
    out[i] = '\0';
}

void reject(const char* source, const char* why) {
    apiLog("[idrv] ");
    apiLog(source);
    apiLog(": ");
    apiLog(why);
    apiLog("\n");
}

// ".idrv" in any case: FAT32 volumes copied to from Windows may hold "PCNET.IDRV".
bool endsWithIdrv(const char* name, size_t length) {
    const char* ext = name + length - 5;
    const char* want = ".idrv";
    for (int i = 0; i < 5; ++i) {
        char c = ext[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != want[i]) {
            return false;
        }
    }
    return true;
}

bool insideImage(uint64_t pointer, uint64_t base, uint64_t size) {
    return pointer >= base && pointer < base + size;
}

}  // namespace

const IdrvKernelApi& ModuleLoader::api() {
    return kApi;
}

size_t ModuleLoader::count() {
    return gModuleCount;
}

const ModuleLoader::Loaded* ModuleLoader::at(size_t index) {
    return index < gModuleCount ? &gModules[index] : nullptr;
}

int ModuleLoader::loadFromMemory(const void* data, size_t size, const char* source, bool requireDevice,
                                 bool atBoot) {
    const auto* file = static_cast<const uint8_t*>(data);
    if (!file || size < sizeof(IdrvFileHeader)) {
        reject(source, "too short");
        return IDRV_EINVAL;
    }
    IdrvFileHeader h;
    memcpy(&h, file, sizeof(h));
    if (h.magic != IDRV_FILE_MAGIC || h.headerSize < sizeof(IdrvFileHeader)) {
        reject(source, "not an .idrv file");
        return IDRV_EINVAL;
    }
    if (h.formatVersion != IDRV_FORMAT_VERSION) {
        reject(source, "unknown format version");
        return IDRV_EINVAL;
    }
    if (h.apiVersion != IDRV_API_VERSION) {
        reject(source, "built for another driver API version");
        return IDRV_EINVAL;
    }
    const uint64_t relocEnd = static_cast<uint64_t>(h.relocOffset) + static_cast<uint64_t>(h.relocCount) * 4;
    const uint64_t imageEnd = static_cast<uint64_t>(h.imageOffset) + h.imageFileSize;
    if (relocEnd > size || imageEnd > size || h.imageFileSize > h.imageSize || h.imageSize == 0 ||
        h.imageSize > 64u * 1024 * 1024 || h.moduleRva + sizeof(IdrvModule) > h.imageFileSize) {
        reject(source, "damaged (sizes)");
        return IDRV_EINVAL;
    }
    if (crc32(file + sizeof(IdrvFileHeader), size - sizeof(IdrvFileHeader)) != h.crc32) {
        reject(source, "damaged (checksum)");
        return IDRV_EINVAL;
    }
    if (gModuleCount >= MAX_MODULES) {
        reject(source, "too many modules");
        return IDRV_ENOMEM;
    }

    // The image runs where the kernel runs: the identity map, above the window
    // a non-PIE program maps over (the direct map is not executable).
    const uint64_t pages = (h.imageSize + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    const uint64_t base = PMM::AllocFramesAbove(pages, PMM::KERNEL_HIGH_ALLOC_MIN);
    if (base == 0) {
        reject(source, "no memory for the image");
        return IDRV_ENOMEM;
    }
    auto* image = reinterpret_cast<uint8_t*>(base);
    memset(image, 0, pages * PMM::PAGE_SIZE);
    memcpy(image, file + h.imageOffset, h.imageFileSize);

    const uint64_t delta = base - h.linkedBase;
    for (uint32_t i = 0; i < h.relocCount; ++i) {
        uint32_t rva;
        memcpy(&rva, file + h.relocOffset + i * 4, 4);
        if (static_cast<uint64_t>(rva) + 8 > h.imageFileSize) {
            PMM::FreeFrames(base, pages);
            reject(source, "damaged (relocation)");
            return IDRV_EINVAL;
        }
        uint64_t value;
        memcpy(&value, image + rva, 8);
        value += delta;
        memcpy(image + rva, &value, 8);
    }

    const auto* module = reinterpret_cast<const IdrvModule*>(image + h.moduleRva);
    const uint64_t imageBytes = h.imageSize;
    const bool sane = module->magic == IDRV_MODULE_MAGIC && module->apiVersion == IDRV_API_VERSION &&
        insideImage(reinterpret_cast<uint64_t>(module->name), base, imageBytes) &&
        (module->pciMatchCount == 0 ||
         (insideImage(reinterpret_cast<uint64_t>(module->pciMatches), base, imageBytes) &&
          module->pciMatchCount <= 256)) &&
        (!module->init || insideImage(reinterpret_cast<uint64_t>(module->init), base, imageBytes)) &&
        (!module->probe || insideImage(reinterpret_cast<uint64_t>(module->probe), base, imageBytes)) &&
        (!module->runtimeApi || insideImage(reinterpret_cast<uint64_t>(module->runtimeApi), base, imageBytes));
    if (!sane) {
        PMM::FreeFrames(base, pages);
        reject(source, "damaged (descriptor)");
        return IDRV_EINVAL;
    }

    PciBus& bus = PciBus::get();
    if (bus.deviceCount() == 0) {
        bus.scan();
    }
    // One module per name: the first found wins (the boot scan reads the
    // disk's /drivers before the initrd's).
    for (size_t i = 0; i < gModuleCount; ++i) {
        if (strcmp(gModules[i].name, module->name) == 0) {
            PMM::FreeFrames(base, pages);
            apiLog("[idrv] ");
            apiLog(source);
            apiLog(": ");
            apiLog(gModules[i].name);
            apiLog(" is already loaded from ");
            apiLog(gModules[i].source);
            apiLog("\n");
            return IDRV_EEXIST;
        }
    }

    size_t matching = 0;
    for (size_t i = 0; i < bus.deviceCount(); ++i) {
        if (moduleMatches(*module, bus.devices()[i]) && !claimed(bus.devices()[i])) {
            ++matching;
        }
    }
    if (matching == 0 && requireDevice) {
        PMM::FreeFrames(base, pages);
        return IDRV_ENODEV;   // nothing for it here: not an error
    }

    Loaded& loaded = gModules[gModuleCount++];
    loaded = {};
    copyName(loaded.name, sizeof(loaded.name), module->name);
    copyName(loaded.version, sizeof(loaded.version),
             insideImage(reinterpret_cast<uint64_t>(module->version), base, imageBytes) ? module->version : "");
    copyName(loaded.description, sizeof(loaded.description),
             insideImage(reinterpret_cast<uint64_t>(module->description), base, imageBytes) ? module->description
                                                                                           : "");
    copyName(loaded.source, sizeof(loaded.source), source);
    loaded.base = base;
    loaded.imageSize = h.imageSize;
    loaded.relocations = h.relocCount;
    loaded.atBoot = atBoot;
    loaded.module = module;

    apiLog("[idrv] ");
    apiLog(loaded.name);
    apiLog(" ");
    apiLog(loaded.version);
    apiLog(" loaded at ");
    logHex(base);
    apiLog(" (");
    logDec(h.imageSize / 1024);
    apiLog(" KiB, ");
    logDec(h.relocCount);
    apiLog(" relocations) from ");
    apiLog(source);
    apiLog("\n");

    if (module->runtimeApi) {
        *module->runtimeApi = &kApi;   // the image is ours: its data is writable
    }
    if (module->init) {
        const int status = module->init(&kApi);
        if (status != IDRV_OK) {
            apiLog("[idrv] ");
            apiLog(loaded.name);
            apiLog(": init failed\n");
            loaded.initFailed = true;
            return status;   // it stays resident: it ran
        }
    }
    if (module->probe) {
        for (size_t i = 0; i < bus.deviceCount(); ++i) {
            const PciDeviceInfo& d = bus.devices()[i];
            if (!moduleMatches(*module, d) || claimed(d)) {
                continue;
            }
            const IdrvPciDevice device = toIdrv(d);
            if (module->probe(&kApi, &device) == IDRV_OK) {
                claim(d);
                ++loaded.devices;
            }
        }
    }
    apiLog("[idrv] ");
    apiLog(loaded.name);
    apiLog(": ");
    logDec(loaded.devices);
    apiLog(loaded.devices == 1 ? " device\n" : " devices\n");
    return loaded.devices > 0 || !requireDevice ? IDRV_OK : IDRV_ENODEV;
}

int ModuleLoader::loadFile(const char* path, bool requireDevice, bool atBoot) {
    FileStats stats = {};
    if (VFS::get().stat(path, &stats) != 0) {
        return IDRV_ENOENT;
    }
    if (stats.type != FileType::Regular || stats.size == 0 || stats.size > 64u * 1024 * 1024) {
        reject(path, "not a module file");
        return IDRV_EINVAL;
    }
    FileDescriptor* fd = nullptr;
    if (VFS::get().open(path, 0, &fd) != 0 || !fd) {
        reject(path, "cannot open it");
        return IDRV_EIO;
    }
    auto* buffer = static_cast<uint8_t*>(kmalloc(stats.size));
    if (!buffer) {
        VFS::get().close(fd);
        return IDRV_ENOMEM;
    }
    uint64_t done = 0;
    while (done < stats.size) {
        const int64_t n = VFS::get().read(fd, buffer + done, stats.size - done);
        if (n <= 0) {
            break;
        }
        done += static_cast<uint64_t>(n);
    }
    VFS::get().close(fd);
    int status = IDRV_EIO;
    if (done == stats.size) {
        status = loadFromMemory(buffer, stats.size, path, requireDevice, atBoot);
    } else {
        reject(path, "short read");
    }
    kfree(buffer);
    return status;
}

void ModuleLoader::loadBootModules(const char* directory) {
    constexpr uint64_t kMaxEntries = 64;
    auto* entries = static_cast<DirEntry*>(kmalloc(sizeof(DirEntry) * kMaxEntries));
    if (!entries) {
        return;
    }
    uint64_t found = 0;
    if (VFS::get().readdir(directory, entries, kMaxEntries, &found) != 0) {
        kfree(entries);
        return;
    }
    size_t loaded = 0;
    size_t modules = 0;
    for (uint64_t i = 0; i < found; ++i) {
        const char* name = entries[i].name;
        const size_t length = strlen(name);
        if (length < 6 || !endsWithIdrv(name, length)) {
            continue;
        }
        ++modules;
        char path[320];
        const size_t dirLength = strlen(directory);
        if (dirLength + 1 + length + 1 > sizeof(path)) {
            continue;
        }
        memcpy(path, directory, dirLength);
        path[dirLength] = '/';
        memcpy(path + dirLength + 1, name, length + 1);
        if (loadFile(path, true, true) == IDRV_OK) {
            ++loaded;
        }
    }
    kfree(entries);
    apiLog("[idrv] boot: ");
    logDec(loaded);
    apiLog(" of ");
    logDec(modules);
    apiLog(" modules in ");
    apiLog(directory);
    apiLog(" took a device\n");
}
