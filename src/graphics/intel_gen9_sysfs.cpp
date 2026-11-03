// The part of Linux's sysfs that libdrm reads to identify a DRM device
// (drmGetDevice2 / drmGetDevices2 in xf86drm.c, which Mesa's loader and
// intel_device_info call): fstat of the node gives 226:128, then
//   /sys/dev/char/226:128/device/drm          the node is a DRM one
//   /sys/dev/char/226:128/device/subsystem     realpath's basename "pci": the bus
//   /sys/dev/char/226:128/device/uevent        PCI_SLOT_NAME=0000:00:02.0
//   /sys/dev/char/226:128/device/{vendor,device,subsystem_vendor,
//                                  subsystem_device,revision}   "0x8086\n" ...
//   /sys/dev/char/226:128/device/config         the configuration space
// laid out as Linux has it (devices/pci0000:00/0000:00:02.0 with drm/renderD128
// under it, links from dev/char, class/drm and bus/pci), written once into
// the RAM file system mounted at /sys. The links are absolute.

#include <graphics/intel_gen9_drm.hpp>

#include <common/string.hpp>
#include <fs/vfs/vfs.hpp>
#include <graphics/intel_gen9.hpp>

namespace drm {

namespace {

bool gPublished = false;

constexpr int kWriteCreate = 0100 | 0x1;   // O_CREAT | O_WRONLY

void writeFile(const char* path, const void* data, uint64_t bytes) {
    FileDescriptor* fd = nullptr;
    if (VFS::get().open(path, kWriteCreate, &fd, 0444) != 0 || !fd) {
        return;
    }
    VFS::get().write(fd, data, bytes);
    VFS::get().close(fd);
}

void writeText(const char* path, const char* text) {
    writeFile(path, text, strlen(text));
}

// "0x%04x\n" (Linux's sysfs format for the PCI id files).
void hexLine(char* out, uint32_t value, int digits) {
    static const char kHex[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < digits; ++i) {
        out[2 + i] = kHex[(value >> ((digits - 1 - i) * 4)) & 0xF];
    }
    out[2 + digits] = '\n';
    out[3 + digits] = '\0';
}

// `digits` hex digits (0: as few as the value needs, as printf's %X).
void hexPlain(char* out, uint32_t value, int digits, bool upper = false) {
    const char* hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (digits == 0) {
        digits = 1;
        while (digits < 8 && (value >> (digits * 4)) != 0) {
            ++digits;
        }
    }
    for (int i = 0; i < digits; ++i) {
        out[i] = hex[(value >> ((digits - 1 - i) * 4)) & 0xF];
    }
    out[digits] = '\0';
}

void append(char* out, const char* text) {
    strcpy(out + strlen(out), text);
}

} // namespace

void publishSysfs() {
    if (gPublished) {
        return;
    }
    intel_gen9::IntelGen9Driver& gpu = intel_gen9::IntelGen9Driver::get();
    const bool real = gpu.isInitialized();
    // Without the GPU (the QEMU checks' DrmWithoutGpu) the layout is the same,
    // the ids zero: there is no device to describe.
    uint32_t config[64] = {};
    uint8_t bus = 0, device = 2, function = 0;
    if (real) {
        for (uint32_t i = 0; i < 64; ++i) {
            config[i] = gpu.pciConfigDword(static_cast<uint16_t>(i * 4));
        }
        bus = gpu.pciBus();
        device = gpu.pciDevice();
        function = gpu.pciFunction();
    }
    gPublished = true;
    const uint32_t vendor = config[0] & 0xFFFF;
    const uint32_t deviceId = config[0] >> 16;
    const uint32_t revision = config[2] & 0xFF;
    const uint32_t classCode = config[2] >> 8;
    const uint32_t subVendor = config[11] & 0xFFFF;
    const uint32_t subDevice = config[11] >> 16;

    // The PCI address as Linux names it: 0000:BB:DD.F.
    char slot[16] = "0000:";
    char two[4];
    hexPlain(two, bus, 2);
    append(slot, two);
    append(slot, ":");
    hexPlain(two, device, 2);
    append(slot, two);
    append(slot, ".");
    two[0] = static_cast<char>('0' + (function & 7));
    two[1] = '\0';
    append(slot, two);

    char dev[128] = "/sys/devices/pci0000:00/";
    append(dev, slot);
    VFS& vfs = VFS::get();
    vfs.mkdir("/sys/devices", 0755);
    vfs.mkdir("/sys/devices/pci0000:00", 0755);
    vfs.mkdir(dev, 0755);

    char path[192];
    char line[16];
    auto at = [&](const char* leaf) {
        strcpy(path, dev);
        append(path, leaf);
        return path;
    };
    hexLine(line, vendor, 4);
    writeText(at("/vendor"), line);
    hexLine(line, deviceId, 4);
    writeText(at("/device"), line);
    hexLine(line, subVendor, 4);
    writeText(at("/subsystem_vendor"), line);
    hexLine(line, subDevice, 4);
    writeText(at("/subsystem_device"), line);
    hexLine(line, revision, 2);
    writeText(at("/revision"), line);
    hexLine(line, classCode, 6);
    writeText(at("/class"), line);
    writeFile(at("/config"), config, sizeof(config));

    char uevent[192] = "DRIVER=i915\nPCI_CLASS=";
    char word[12];
    // As Linux's pci_uevent prints them: PCI_CLASS=%X, PCI_ID=%04X:%04X.
    hexPlain(word, classCode, 0, true);
    append(uevent, word);
    append(uevent, "\nPCI_ID=");
    hexPlain(word, vendor, 4, true);
    append(uevent, word);
    append(uevent, ":");
    hexPlain(word, deviceId, 4, true);
    append(uevent, word);
    append(uevent, "\nPCI_SUBSYS_ID=");
    hexPlain(word, subVendor, 4, true);
    append(uevent, word);
    append(uevent, ":");
    hexPlain(word, subDevice, 4, true);
    append(uevent, word);
    append(uevent, "\nPCI_SLOT_NAME=");
    append(uevent, slot);
    append(uevent, "\n");
    writeText(at("/uevent"), uevent);

    vfs.mkdir("/sys/bus", 0755);
    vfs.mkdir("/sys/bus/pci", 0755);
    vfs.mkdir("/sys/bus/pci/devices", 0755);
    vfs.mkdir("/sys/bus/pci/drivers", 0755);
    vfs.mkdir("/sys/bus/pci/drivers/i915", 0755);
    vfs.symlink("/sys/bus/pci", at("/subsystem"));
    vfs.symlink("/sys/bus/pci/drivers/i915", at("/driver"));
    char busLink[96] = "/sys/bus/pci/devices/";
    append(busLink, slot);
    vfs.symlink(dev, busLink);

    // The render node's own directory.
    vfs.mkdir(at("/drm"), 0755);
    vfs.mkdir(at("/drm/renderD128"), 0755);
    writeText(at("/drm/renderD128/dev"), "226:128\n");
    writeText(at("/drm/renderD128/uevent"), "MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\nDEVTYPE=drm_minor\n");
    vfs.symlink(dev, at("/drm/renderD128/device"));
    vfs.mkdir("/sys/class", 0755);
    vfs.mkdir("/sys/class/drm", 0755);
    vfs.symlink("/sys/class/drm", at("/drm/renderD128/subsystem"));
    char node[160];
    strcpy(node, dev);
    append(node, "/drm/renderD128");
    vfs.symlink(node, "/sys/class/drm/renderD128");
    vfs.mkdir("/sys/dev", 0755);
    vfs.mkdir("/sys/dev/char", 0755);
    vfs.symlink(node, "/sys/dev/char/226:128");
}

} // namespace drm
