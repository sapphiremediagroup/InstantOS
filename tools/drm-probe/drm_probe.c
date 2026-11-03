// drm-probe: /dev/dri/renderD128 through the real libdrm (tools/build-libdrm.sh),
// as Mesa reaches it: device enumeration and identification (drmGetDevices2,
// drmGetDevice2, the node and its name), drmGetVersion and drmGetCap, sync
// objects, sync_files and timelines through libdrm's helpers, and PRIME
// (drmPrimeHandleToFD, lseek, mmap, drmPrimeFDToHandle in a second open).
// The fds libdrm hands back are mlibc's own: its ioctl wrapper translates
// those in DRM structs. Without an Intel GPU (QEMU, after `gpu-test gem
// cpu-only`) the ids are zero and only the layout is checked.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <i915_drm.h>

static int failures;

static void check(int ok, const char *what) {
    printf("drm-probe: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

int main(void) {
    drmDevicePtr devices[8];
    const int count = drmGetDevices2(0, devices, 8);
    int found = -1;
    for (int i = 0; i < count; i++) {
        if ((devices[i]->available_nodes & (1 << DRM_NODE_RENDER)) &&
            strcmp(devices[i]->nodes[DRM_NODE_RENDER], "/dev/dri/renderD128") == 0) {
            found = i;
        }
    }
    check(count >= 1 && found >= 0 && devices[found]->bustype == DRM_BUS_PCI,
          "drmGetDevices2: a PCI device with render node /dev/dri/renderD128");
    if (found < 0) {
        printf("drm-probe: some checks FAILED (%d devices)\n", count);
        return 1;
    }
    const drmPciDeviceInfo *pci = devices[found]->deviceinfo.pci;
    const drmPciBusInfo *bus = devices[found]->businfo.pci;
    const int real = pci->vendor_id != 0;
    printf("drm-probe: %04x:%02x:%02x.%u vendor %04x device %04x subsystem %04x:%04x\n", bus->domain, bus->bus,
           bus->dev, bus->func, pci->vendor_id, pci->device_id, pci->subvendor_id, pci->subdevice_id);
    check(bus->domain == 0 && bus->bus == 0 && bus->dev == 2 && bus->func == 0, "its bus address 0000:00:02.0");

    const int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    check(fd >= 0 && fd < 256, "open: a small fd");
    drmDevicePtr mine = NULL;
    check(drmGetDevice2(fd, DRM_DEVICE_GET_PCI_REVISION, &mine) == 0 && mine &&
              drmDevicesEqual(mine, devices[found]),
          "drmGetDevice2 on the fd: the same device (Mesa's loader and intel_device_info)");
    if (mine && real) {
        printf("drm-probe: revision %u\n", mine->deviceinfo.pci->revision_id);
        check(mine->deviceinfo.pci->vendor_id == 0x8086, "vendor Intel");
    }
    check(drmGetNodeTypeFromFd(fd) == DRM_NODE_RENDER, "drmGetNodeTypeFromFd: a render node");
    char *name = drmGetRenderDeviceNameFromFd(fd);
    check(name && strcmp(name, "/dev/dri/renderD128") == 0, "drmGetRenderDeviceNameFromFd");
    drmVersionPtr version = drmGetVersion(fd);
    check(version && strcmp(version->name, "i915") == 0, "drmGetVersion: i915");
    uint64_t cap = 0;
    check(drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0 && cap == 1 && drmGetCap(fd, DRM_CAP_PRIME, &cap) == 0 &&
              cap == (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT),
          "drmGetCap: timeline sync objects, PRIME");

    // Sync objects and sync_files.
    uint32_t signaled = 0, empty = 0, timeline = 0;
    check(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &signaled) == 0 && drmSyncobjCreate(fd, 0, &empty) == 0 &&
              drmSyncobjCreate(fd, 0, &timeline) == 0,
          "drmSyncobjCreate");
    int syncFile = -1;
    check(drmSyncobjExportSyncFile(fd, signaled, &syncFile) == 0 && syncFile >= 0 && syncFile < 256,
          "drmSyncobjExportSyncFile: a small fd");
    struct pollfd p = {syncFile, POLLIN, 0};
    check(poll(&p, 1, 0) == 1 && (p.revents & POLLIN), "poll on the sync_file: signaled");
    check(drmSyncobjImportSyncFile(fd, empty, syncFile) == 0 && drmSyncobjWait(fd, &empty, 1, 0, 0, NULL) == 0,
          "drmSyncobjImportSyncFile, then drmSyncobjWait");
    check(close(syncFile) == 0, "close the sync_file");
    int opaque = -1;
    uint32_t again = 0;
    check(drmSyncobjHandleToFD(fd, timeline, &opaque) == 0 && drmSyncobjFDToHandle(fd, opaque, &again) == 0 && again != 0,
          "drmSyncobjHandleToFD / FDToHandle");
    uint64_t point = 5, reached = 0;
    check(drmSyncobjTimelineSignal(fd, &again, &point, 1) == 0 && drmSyncobjQuery(fd, &timeline, &reached, 1) == 0 &&
              reached == 5,
          "drmSyncobjTimelineSignal 5 through one handle, drmSyncobjQuery 5 through the other");
    point = 4;
    check(drmSyncobjTimelineWait(fd, &timeline, &point, 1, 0, 0, NULL) == 0, "drmSyncobjTimelineWait for 4");
    close(opaque);

    // PRIME: an object out as a dma-buf, sized, mapped, and in again elsewhere.
    struct drm_i915_gem_create create = {.size = 2 * 4096};
    check(drmIoctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create) == 0, "GEM_CREATE through drmIoctl");
    int dmabuf = -1;
    check(drmPrimeHandleToFD(fd, create.handle, DRM_CLOEXEC | DRM_RDWR, &dmabuf) == 0 && dmabuf >= 0 && dmabuf < 256,
          "drmPrimeHandleToFD: a small fd");
    check(lseek(dmabuf, 0, SEEK_END) == 2 * 4096, "lseek(SEEK_END) on the dma-buf: its size");
    volatile uint32_t *map = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf, 0);
    check(map != MAP_FAILED, "mmap of the dma-buf");
    const int second = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    uint32_t imported = 0;
    check(second >= 0 && drmPrimeFDToHandle(second, dmabuf, &imported) == 0 && imported != 0,
          "drmPrimeFDToHandle in a second open");
    if (map != MAP_FAILED && imported) {
        map[1024] = 0xD7AB0001;
        struct drm_i915_gem_mmap_offset offset = {.handle = imported, .flags = I915_MMAP_OFFSET_WB};
        volatile uint32_t *other = MAP_FAILED;
        if (drmIoctl(second, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &offset) == 0) {
            other = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, second, offset.offset);
        }
        check(other != MAP_FAILED && other[1024] == 0xD7AB0001, "the other open's mapping shows the same page");
    }
    close(dmabuf);
    close(second);
    close(fd);
    drmFreeDevices(devices, count);
    printf(failures == 0 ? "drm-probe: all checks passed\n" : "drm-probe: some checks FAILED\n");
    return failures == 0 ? 0 : 1;
}
