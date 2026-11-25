#pragma once

/*
 * InstantOS loadable drivers: the .idrv format and the kernel's driver API.
 *
 * A driver is built like the kernel (clang -target x86_64-unknown-windows, the
 * same freestanding flags) and linked with lld-link as a DLL with no imports;
 * tools/mkidrv turns that DLL into a .idrv file. The kernel (module_loader.cpp)
 * copies the image anywhere, applies its relocations (64-bit absolute
 * pointers), reads its IdrvModule descriptor and calls init() with the
 * IdrvKernelApi table: a driver reaches the kernel only through that table, so
 * it keeps working when the kernel's own classes change. The table only grows
 * (new members at the end, `size` tells how far it goes); a change that is not
 * an addition bumps IDRV_API_VERSION, and the kernel refuses a module built
 * against another version.
 *
 * This header is C and C++, and holds only fixed-size types: it is the ABI.
 * docs/idrv.md describes the format, the loader and how to write a driver.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IDRV_API_VERSION 1u
#define IDRV_FORMAT_VERSION 1u
#define IDRV_FILE_MAGIC 0x56524449u    /* "IDRV" */
#define IDRV_MODULE_MAGIC 0x444F4D49u  /* "IMOD" */

/* ---- The file ---------------------------------------------------------- */

/*
 * Offset 0 of a .idrv file. All offsets are from the start of the file, all
 * RVAs from the start of the image, little endian. The image (imageFileSize
 * bytes at imageOffset, zero-filled to imageSize) was linked to run at
 * linkedBase; each reloc entry is the RVA of a 64-bit pointer in it, which the
 * loader moves by (load address - linkedBase). crc32 (IEEE, as zlib's) covers
 * every byte after the header.
 */
typedef struct IdrvFileHeader {
    uint32_t magic;          /* IDRV_FILE_MAGIC */
    uint16_t formatVersion;  /* IDRV_FORMAT_VERSION */
    uint16_t headerSize;     /* sizeof(IdrvFileHeader) */
    uint32_t apiVersion;     /* IDRV_API_VERSION it was built against */
    uint32_t flags;          /* none yet, 0 */
    uint64_t linkedBase;
    uint32_t imageSize;      /* bytes to reserve, a multiple of 4096 */
    uint32_t imageFileSize;  /* bytes stored */
    uint32_t imageOffset;
    uint32_t relocCount;
    uint32_t relocOffset;    /* relocCount uint32_t RVAs */
    uint32_t moduleRva;      /* the IdrvModule descriptor */
    uint32_t textRva;        /* the code, for the log and a later W^X */
    uint32_t textSize;
    uint32_t crc32;
    uint32_t reserved;
} IdrvFileHeader;

/* ---- What the module describes about itself --------------------------- */

typedef struct IdrvPciDevice {
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t classCode;
    uint8_t subclass;
    uint8_t progIf;
    uint16_t vendorId;
    uint16_t deviceId;
    uint8_t revision;
    uint8_t interruptPin;    /* 0 none, 1..4 INTA..INTD */
    uint16_t reserved;
} IdrvPciDevice;

#define IDRV_PCI_ANY 0xFFFFu

/*
 * A device the driver takes: vendor and device (IDRV_PCI_ANY for any), and
 * (class << 16 | subclass << 8 | progIf) & classMask == classValue.
 */
typedef struct IdrvPciMatch {
    uint16_t vendorId;
    uint16_t deviceId;
    uint32_t classMask;
    uint32_t classValue;
} IdrvPciMatch;

struct IdrvKernelApi;

/* Status codes: 0 is success, negative is an errno-style failure. */
#define IDRV_OK 0
#define IDRV_ENOENT (-2)
#define IDRV_EEXIST (-17)
#define IDRV_ENODEV (-19)
#define IDRV_ENOMEM (-12)
#define IDRV_EIO (-5)
#define IDRV_EINVAL (-22)

typedef struct IdrvModule {
    uint32_t magic;          /* IDRV_MODULE_MAGIC */
    uint32_t apiVersion;     /* IDRV_API_VERSION */
    const char* name;        /* short, as in the log: "pcnet" */
    const char* version;
    const char* description;
    const char* license;     /* an SPDX id, or the project's name */
    const IdrvPciMatch* pciMatches;
    uint32_t pciMatchCount;
    uint32_t reserved;
    /* Once, after loading; nonzero unloads it again. May be null. */
    int (*init)(const struct IdrvKernelApi* api);
    /* For each matching device, in bus order; 0 means it took the device. */
    int (*probe)(const struct IdrvKernelApi* api, const IdrvPciDevice* device);
    /* Before an unload (not done yet). May be null. */
    void (*exit)(void);
    /* Where the loader stores the API before init, for the module's runtime
     * (idrv_runtime.cpp: its memcpy/memset use the kernel's, which the kernel
     * picks per CPU at boot). IDRV_RUNTIME_API fills it in. May be null. */
    const struct IdrvKernelApi** runtimeApi;
} IdrvModule;

/* The one symbol a module exports. */
#if defined(_WIN32) || defined(__MINGW32__) || defined(_MSC_VER) || defined(__CYGWIN__) || \
    defined(IDRV_BUILDING_MODULE)
#define IDRV_EXPORT __declspec(dllexport)
#else
#define IDRV_EXPORT
#endif
#define IDRV_MODULE_SYMBOL idrv_module

/* ---- Callbacks the kernel calls ---------------------------------------- */

/* irq_register's result. */
#define IDRV_IRQ_NONE 0      /* no interrupt: the driver is polled */
#define IDRV_IRQ_MSI 1
#define IDRV_IRQ_LEGACY 2    /* a shared INTx line: claim() is asked first */
#define IDRV_IRQ_MSIX 3

/* irq_register flags. */
#define IDRV_IRQ_ALLOW_MSI (1u << 0)
#define IDRV_IRQ_ALLOW_LEGACY (1u << 1)

/* What a handler asks the kernel to do after it. */
#define IDRV_IRQ_DONE 0
#define IDRV_IRQ_NET_RECEIVE (1u << 0)   /* drain the network device's receive ring */

typedef struct IdrvIrqOps {
    /* Acknowledges the device's interrupt causes; on a shared INTx line its
     * answer also says whether this device raised it (MSI ignores it). May be
     * null for an MSI-only driver. */
    int (*claim)(void* context);
    /* Runs with interrupts off under the big kernel lock; the kernel sends
     * the EOI. Returns IDRV_IRQ_* requests. */
    uint32_t (*handle)(void* context);
} IdrvIrqOps;

/* A network device, as the kernel's NetDevice. Frames are Ethernet, without
 * the FCS, at most 1514 bytes. */
typedef struct IdrvNetOps {
    const char* name;        /* as netcfg shows it */
    int32_t probePriority;   /* lower first; the kernel's NICs use 10-90 */
    uint32_t reserved;
    void (*macAddress)(void* context, uint8_t mac[6]);
    int (*send)(void* context, const void* frame, size_t length);   /* nonzero if queued */
    int (*receive)(void* context, void* buffer, size_t maxLength);  /* bytes, or -1 */
    int (*linkUp)(void* context);
    int (*interruptDriven)(void* context);   /* nonzero: it raises IDRV_IRQ_NET_RECEIVE */
} IdrvNetOps;

/* ---- The kernel's side ------------------------------------------------- */

typedef struct IdrvKernelApi {
    uint32_t version;        /* IDRV_API_VERSION */
    uint32_t size;           /* sizeof(IdrvKernelApi) of the kernel */

    /* The kernel log (serial port and the dmesg ring). */
    void (*log)(const char* text);
    void (*logHex)(uint64_t value);
    void (*logDec)(uint64_t value);

    /* Time. delayUs spins; it is for short waits in set-up. */
    uint64_t (*uptimeNs)(void);
    void (*delayUs)(uint64_t microseconds);

    /* The kernel heap. */
    void* (*alloc)(size_t bytes);
    void (*free)(void* pointer);

    /* Zeroed physically contiguous pages for DMA; *virt is where the CPU
     * reaches them. maxPhys 0 means anywhere, else every byte below it. */
    uint64_t (*dmaAlloc)(uint64_t pages, uint64_t maxPhys, void** virt);
    void (*dmaFree)(uint64_t phys, uint64_t pages);

    /* PCI configuration space; width 1, 2 or 4. */
    uint32_t (*pciRead)(const IdrvPciDevice* device, uint16_t offset, uint32_t width);
    void (*pciWrite)(const IdrvPciDevice* device, uint16_t offset, uint32_t width, uint32_t value);
    /* A memory BAR mapped uncached (at least `bytes`); 0 if it is not one. */
    uint64_t (*pciMapBar)(const IdrvPciDevice* device, uint32_t bar, uint64_t bytes);

    /* Hooks the device's interrupt: MSI if allowed and present, else its
     * INTx line. Returns IDRV_IRQ_*. ops and context must stay valid. */
    uint32_t (*irqRegister)(const IdrvPciDevice* device, uint32_t flags, const IdrvIrqOps* ops,
                            void* context);

    /* Adds a network device; the stack picks one by probePriority. */
    int (*netRegister)(const IdrvNetOps* ops, void* context);

    /* The kernel's copy and fill (rep, SSE2 or AVX2, whichever this CPU runs
     * fastest: rep is emulated a byte at a time under QEMU's TCG). */
    void* (*memcpy)(void* dst, const void* src, size_t n);
    void* (*memset)(void* dst, int value, size_t n);

    /* Added in step 3 (a module that needs them checks `size`). */

    /* Hooks MSI-X table entry `entry` (the device decides which of its events
     * use it, e.g. virtio's queue_msix_vector) and enables MSI-X. Returns
     * IDRV_IRQ_MSIX or IDRV_IRQ_NONE. As irqRegister, the kernel sends the EOI
     * and the vector goes to whichever CPU IRQ balancing picks. */
    uint32_t (*irqRegisterMsix)(const IdrvPciDevice* device, uint32_t entry, const IdrvIrqOps* ops,
                                void* context);

    /* Bytes from the kernel's CSPRNG (as getentropy). */
    void (*random)(void* buffer, size_t length);
} IdrvKernelApi;

/* Whether a kernel's table reaches `member` (a module built against a newer
 * header may run on an older kernel of the same version). */
#define IDRV_API_HAS(api, member) \
    ((api)->size >= offsetof(IdrvKernelApi, member) + sizeof((api)->member))

/* For IdrvModule.runtimeApi: the SDK runtime's slot. */
#ifdef __cplusplus
extern "C" const IdrvKernelApi* idrv_runtime_api;
#else
extern const IdrvKernelApi* idrv_runtime_api;
#endif
#define IDRV_RUNTIME_API (&idrv_runtime_api)

#ifdef __cplusplus
}
#endif
