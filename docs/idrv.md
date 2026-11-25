# Loadable drivers (.idrv)

InstantOS drivers can live outside the kernel image as `.idrv` modules. The
kernel loads them at boot from `/lib/drivers` (the initrd), for the PCI devices
they say they handle. A module talks to the kernel only through a versioned
table of functions, so the kernel can change its own classes without breaking
modules. Every PCI network driver is a module: `virtio-net`, `e1000` (Intel
8254x through I219, igb, I225/I226), `rtl8169` (RTL8169/8111/8168/8101,
RTL8125), `vmxnet3`, `rtl8139` and `pcnet`. USB Ethernet stays in the kernel
until there is a USB API for modules.

Porting from the kernel, mind two things. The kernel's drivers used a frame's
physical address as its pointer (the identity map), but `dmaAlloc` gives the CPU
a direct-map address, so keep the physical one apart for the device. And a module
runs no constructors: keep globals plain (constant-initialized or zero).

## Pieces

| Where | What |
|---|---|
| `include/idrv/idrv.h` | The ABI: file header, module descriptor, PCI match table, callback tables, the kernel API. C and C++. |
| `modules/sdk/` | `idrv_sdk.h` (port I/O, MMIO, barriers) and `idrv_runtime.cpp` (memcpy/memset/memmove/memcmp), linked into every module. |
| `modules/<name>/` | A driver's sources. |
| `tools/mkidrv` | Host tool: PE32+ DLL to `.idrv`; `mkidrv --info x.idrv` prints a module. |
| `src/drivers/modules/module_loader.cpp` | Checks, places, relocates and starts modules; implements the API. |
| `CMakeLists.txt` `add_idrv_module(name sources...)` | Builds `build/drivers/<name>.idrv` and puts it in the initrd as `lib/drivers/<name>.idrv`. `idrv_modules` builds them all. |

## Building a module

A module is compiled like the kernel (clang `-target x86_64-unknown-windows`,
freestanding, no RTTI or exceptions, the same Windows x64 calling convention)
plus `-mno-stack-arg-probe`. It is linked by lld-link as a DLL
(`/dll /noentry /base:0x180000000`, no default libraries) that **imports
nothing**: `mkidrv` refuses a DLL with an import table. It exports one symbol:

```cpp
#include "../sdk/idrv_sdk.h"

extern "C" IDRV_EXPORT const IdrvModule idrv_module = {
    IDRV_MODULE_MAGIC, IDRV_API_VERSION,
    "name", "1.0", "description", "license",
    kMatches, sizeof(kMatches) / sizeof(kMatches[0]), 0,
    init,    // int(const IdrvKernelApi*), once; may be null
    probe,   // int(const IdrvKernelApi*, const IdrvPciDevice*), per matching device
    exit,    // void(), before an unload (not done yet); may be null
    IDRV_RUNTIME_API,   // the loader gives the SDK runtime the kernel API here
};
```

Globals need no constructors: the loader runs none. Keep them constant-initialized
(plain data, pointers to functions and strings) or zero.

## The file

```
offset 0             IdrvFileHeader (64 bytes)
headerSize           relocCount x uint32_t: RVAs of 64-bit pointers
imageOffset (16-al.) the image, imageFileSize bytes (zero-filled to imageSize in memory)
```

The image is the DLL as it lies in memory, made from its sections. The
discardable ones (`.reloc`) are left out. `linkedBase` is the DLL's ImageBase.
x86-64 PE files only have DIR64 base relocations, so each relocation is a pointer
the loader moves by `load address - linkedBase`. `crc32` (IEEE, as zlib) covers
everything after the header. `textRva`/`textSize` mark the code.

## Loading

`ModuleLoader::loadBootModules("/lib/drivers")` runs in `main.cpp` once `/lib`
is mounted, before the userland services start (and so before anything asks
for a NIC). For each `*.idrv` file it:

1. checks the magic, format and API versions, the sizes and the CRC;
2. takes contiguous frames above `PMM::KERNEL_HIGH_ALLOC_MIN` and runs the
   image through the identity map, as the kernel itself runs. The direct map is
   NX, and a non-PIE program maps itself over the identity addresses below that
   floor;
3. copies the image, applies the relocations and checks the descriptor (magic,
   version, pointers inside the image);
4. unloads it again if no PCI device matches its table;
5. calls `init`, then `probe` for every matching device, in bus order.

A module that has run is never freed (it may have left callbacks with the
kernel); unloading needs `exit` and the kernel undoing what it registered.

The boot scan reads the root disk's `/drivers` first, then the initrd's
`/lib/drivers`. Only one module of a name is loaded (`IDRV_EEXIST` for the
next), so a module dropped into `/drivers` replaces the built-in one: a driver
update without a new initrd. On the PC that is `F:\drivers` from Windows (the
FAT32 driver reads long names, and the extension may be in any case). A PCI
function a module's probe took is not offered to any other module.

## At run time

- `lsmod` (`ModuleList`, syscall 163) lists the loaded modules: name, version,
  devices, size and the file each came from. `lsmod -v` adds the description,
  load address, relocations and whether it came at boot.
- `modload FILE` (`ModuleLoad`, 164, root only) loads one module for the devices
  it matches, the same way as at boot. It fails with `ENOENT`, `EINVAL` (not a
  module, damaged, or another API version), `EEXIST` or `ENODEV`. `dmesg | grep idrv`
  tells why.
- A NIC module loaded at run time joins the stack's registry, but the stack keeps
  the NIC it already uses. It is picked when none was found at boot.

The feature suite's `test_modules` loads `modules/idrvtest`, a test-only module
(`add_idrv_module(idrvtest TEST ...)`, packed as `/lib/idrv-test/idrvtest.idrv`).
It copies the module to the ext4 root disk and loads it from there, checks
`ModuleList`, `EEXIST`, `ENOENT`, a damaged copy and a module without its device,
and finds the module's own API checks in the kernel log. With `IDRV_ON_DISK=1`,
`build-testsuite-iso.sh` seeds the disk's `/drivers` with it instead, and the
same test checks that the boot scan took it from there.
The log has `[idrv] <name> <version> loaded at ...` and `[idrv] <name>: N devices`.

## The kernel API (version 1)

`IdrvKernelApi` grows only at its end, and `size` says how far a kernel's
goes. Any other change bumps `IDRV_API_VERSION`, and the loader refuses modules
built against another version. Version 1 has:

- the log (`log`, `logHex`, `logDec`);
- time (`uptimeNs`, a spinning `delayUs`);
- the kernel heap (`alloc`, `free`);
- DMA pages (`dmaAlloc(pages, maxPhys, &virt)`, zeroed and contiguous; `dmaFree`);
- PCI configuration space (`pciRead`, `pciWrite`) and an uncached BAR mapping (`pciMapBar`);
- interrupts: `irqRegister(device, IDRV_IRQ_ALLOW_MSI | IDRV_IRQ_ALLOW_LEGACY, ops, context)`
  returns `IDRV_IRQ_MSI`, `IDRV_IRQ_LEGACY` or `IDRV_IRQ_NONE`. `claim` acknowledges
  the device (and, on a shared INTx line, says whether it raised it). `handle`
  returns `IDRV_IRQ_NET_RECEIVE` to have the stack drain the NIC. The kernel
  sends the EOI;
- network devices: `netRegister(ops, context)` adds a NIC to the stack's
  registry, picked by `probePriority` as the built-in ones are.
- `memcpy`/`memset`: the kernel's, which it picks per CPU at boot (rep on real
  CPUs, SIMD under QEMU's TCG, where rep runs a byte at a time). The SDK runtime
  uses them from 64 bytes once the loader has stored the API in `runtimeApi`.
- added in step 3, at the end: `irqRegisterMsix(device, entry, ops, context)`
  (`IDRV_IRQ_MSIX`; virtio-net's receive queue) and `random(buffer, length)`
  (the kernel's CSPRNG, for a made-up MAC address). A module checks for later
  members with `IDRV_API_HAS(api, member)`, since `size` says how much of the
  table the running kernel has.

Callbacks run under the big kernel lock, with interrupts off, as kernel drivers do.
