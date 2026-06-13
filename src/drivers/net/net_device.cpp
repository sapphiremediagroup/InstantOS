#include <drivers/net/net_device.hpp>
#include <drivers/virtio/virtio_net.hpp>
#include <drivers/net/rtl8139.hpp>
#include <drivers/net/e1000.hpp>

namespace {
NetDevice* gDevices[NetDeviceRegistry::MAX_DEVICES] = {};
size_t gDeviceCount = 0;
NetDevice* gActive = nullptr;
bool gActiveResolved = false;
bool gDriversRegistered = false;

// Register the built-in NIC drivers.  Done lazily on first use rather than via
// static-init constructors because the kernel does not run init_array global
// constructors — a file-scope object's constructor would never execute, so the
// registry would stay empty.  Each ::get() is a function-local (Meyers)
// singleton, which IS initialized on first call, so this is safe.
void ensureDriversRegistered() {
    if (gDriversRegistered) {
        return;
    }
    gDriversRegistered = true;
    NetDeviceRegistry::registerDevice(&VirtIONetDriver::get());
    NetDeviceRegistry::registerDevice(&E1000Driver::get());
    NetDeviceRegistry::registerDevice(&RTL8139Driver::get());
}
}

void NetDeviceRegistry::registerDevice(NetDevice* device) {
    if (!device) {
        return;
    }
    for (size_t i = 0; i < gDeviceCount; ++i) {
        if (gDevices[i] == device) {
            return;  // already registered
        }
    }
    if (gDeviceCount >= MAX_DEVICES) {
        return;
    }
    gDevices[gDeviceCount++] = device;
    // A newly registered driver might be the one that should be active.
    gActiveResolved = false;
}

NetDevice* NetDeviceRegistry::active() {
    ensureDriversRegistered();
    if (gActiveResolved && gActive) {
        return gActive;
    }

    // First pass: prefer a device that is already initialized.
    for (size_t i = 0; i < gDeviceCount; ++i) {
        if (gDevices[i] && gDevices[i]->isInitialized()) {
            gActive = gDevices[i];
            gActiveResolved = true;
            return gActive;
        }
    }

    // Second pass: probe registered drivers in ascending priority order and
    // use the first one whose hardware is present and initializes
    // successfully.  Selection-sort over the small fixed table keeps this
    // allocation-free and order-independent of static init.
    bool tried[MAX_DEVICES] = {};
    for (size_t pass = 0; pass < gDeviceCount; ++pass) {
        size_t best = MAX_DEVICES;
        for (size_t i = 0; i < gDeviceCount; ++i) {
            if (tried[i] || !gDevices[i]) {
                continue;
            }
            if (best == MAX_DEVICES ||
                gDevices[i]->probePriority() < gDevices[best]->probePriority()) {
                best = i;
            }
        }
        if (best == MAX_DEVICES) {
            break;
        }
        tried[best] = true;
        if (gDevices[best]->initialize()) {
            gActive = gDevices[best];
            gActiveResolved = true;
            return gActive;
        }
    }

    gActive = nullptr;
    gActiveResolved = true;
    return nullptr;
}

size_t NetDeviceRegistry::count() {
    return gDeviceCount;
}

NetDevice* NetDeviceRegistry::deviceAt(size_t index) {
    if (index >= gDeviceCount) {
        return nullptr;
    }
    return gDevices[index];
}
