// idrvtest: a module for the feature suite (tools/os-testsuite, test_modules),
// not part of the system image. It takes the PCI host bridge, which every
// QEMU machine has, without touching it, and checks the kernel API from
// inside a module; runner.c reads its verdict from the kernel log.

#include "../sdk/idrv_sdk.h"

namespace {

const IdrvKernelApi* gApi;
uint32_t gInitBad;   // nonzero: which check failed

int init(const IdrvKernelApi* api) {
    gApi = api;
    if (api->version != IDRV_API_VERSION || api->size < sizeof(IdrvKernelApi)) {
        gInitBad = 1;
    }
    // The heap.
    auto* block = static_cast<uint8_t*>(api->alloc(300));
    if (!block) {
        gInitBad = 2;
    } else {
        // Copies of 64 bytes and more go through the kernel's memcpy/memset.
        memset(block, 0x5A, 300);
        uint8_t copy[300];
        memcpy(copy, block, sizeof(copy));
        for (int i = 0; i < 300; ++i) {
            if (copy[i] != 0x5A) {
                gInitBad = 3;
            }
        }
        api->free(block);
    }
    // DMA pages below 4 GiB, zeroed, reachable at virt.
    void* virt = nullptr;
    const uint64_t phys = api->dmaAlloc(2, 0x100000000ULL, &virt);
    if (phys == 0 || phys + 2 * 4096 > 0x100000000ULL || !virt ||
        static_cast<uint8_t*>(virt)[8191] != 0) {
        gInitBad = 4;
    } else {
        static_cast<uint8_t*>(virt)[100] = 1;
        api->dmaFree(phys, 2);
    }
    // The delay ends, after at least its time by the uptime (which may stand
    // still meanwhile, see SMP::delayUs).
    const uint64_t t0 = api->uptimeNs();
    api->delayUs(200);
    const uint64_t took = api->uptimeNs() - t0;
    if (took != 0 && took < 200000) {
        gInitBad = 5;
    }
    api->log(gInitBad ? "[idrvtest] init: FAILED check " : "[idrvtest] init: ok, api ");
    api->logDec(gInitBad ? gInitBad : api->version);
    api->log("\n");
    return gInitBad ? IDRV_EIO : IDRV_OK;
}

int probe(const IdrvKernelApi* api, const IdrvPciDevice* dev) {
    const uint32_t ids = api->pciRead(dev, 0, 4);
    const bool same = (ids & 0xFFFF) == dev->vendorId && (ids >> 16) == dev->deviceId;
    api->log(same ? "[idrvtest] probe: host bridge " : "[idrvtest] probe: config space MISMATCH ");
    api->logHex(dev->vendorId);
    api->log(":");
    api->logHex(dev->deviceId);
    api->log("\n");
    return same ? IDRV_OK : IDRV_ENODEV;
}

const IdrvPciMatch kMatches[] = {
    {IDRV_PCI_ANY, IDRV_PCI_ANY, 0xFFFF00u, 0x060000u},   // class 06 (bridge), subclass 00 (host)
};

}  // namespace

extern "C" IDRV_EXPORT const IdrvModule idrv_module = {
    IDRV_MODULE_MAGIC,
    IDRV_API_VERSION,
    "idrvtest",
    "1.0",
    "feature suite test module (takes the PCI host bridge, does nothing)",
    "InstantOS",
    kMatches,
    sizeof(kMatches) / sizeof(kMatches[0]),
    0,
    init,
    probe,
    nullptr,
    IDRV_RUNTIME_API,
};
