# Venus (Vulkan over virtio-gpu)

InstantOS implements the guest side of **Venus**, Mesa's Vulkan-over-virtio-gpu
protocol (`VK_MESA_venus_protocol`, virtio-gpu capset id 4). Venus rides on the
existing virtio-gpu 3D plane (the same `VirtIOGPUDriver` that powers VirGL) and
lets the guest drive a real host Vulkan implementation through the QEMU
virtio-gpu device.

## What is implemented

The kernel-side Venus layer lives in `include/graphics/venus.hpp` and
`src/graphics/venus.cpp` (the `venus::Venus` singleton). It provides:

- **Capset negotiation** — enumerates the device capsets, finds the Venus capset
  (id 4), fetches and parses its payload (`VirtIOGPUVenusCapset`), and validates
  the wire-format version (must be 1) and the advertised Vulkan `vk.xml`
  version. `VirtIOGPUDriver::detectVenusCapset()` does the discovery;
  `Venus::negotiate()` does the validation and caches the result.
- **Venus context creation** — `Venus::createContext()` issues a virtio-gpu
  `CTX_CREATE` with `context_init` carrying capset id 4
  (`createContextWithCapset`), which makes the host create a Venus (`vkr`)
  renderer context.
- **Shared command/reply memory** — `Venus::allocateSharedBlob()` allocates a
  `BLOB_MEM_HOST3D` blob with `blob_id 0` inside the Venus context
  (`VirtIOGPUDriver::allocateContextBlob`) and maps it into the guest through the
  host-visible SHM window (`RESOURCE_MAP_BLOB`). This mirrors how Mesa's own
  Venus driver allocates its CS/reply shmem (see `virtgpu_init_shmem_blob_mem`).
- **A command-stream (CS) encoder/decoder** — `venus::CsEncoder` /
  `venus::CsDecoder` implement the Venus little-endian wire format
  (4-byte scalars; 8-byte `uint64_t`, `size_t`, "array size" and
  "simple pointer" presence values; the cursor advances in 4-byte steps).
- **A real synchronous round trip** — `Venus::queryInstanceVersion()` encodes
  `vkSetReplyCommandStreamMESA` + `vkSeekReplyCommandStreamMESA` +
  `vkExecuteCommandStreamsMESA` wrapping a `vkEnumerateInstanceVersion` command
  (with `GENERATE_REPLY`), submits it via `SUBMIT_3D`, waits on the fence, polls
  the reply region for the asynchronously written reply, and decodes the Vulkan
  instance version reported by the host.
- **An asynchronous command ring** — `venus::VulkanSession` creates a Venus ring
  with `vkCreateRingMESA` (a `VkRingCreateInfoMESA` describing the head/tail/
  status/buffer offsets inside a shared HOST3D blob). Commands are written into
  the circular ring buffer, the tail is published, and the host is woken with
  `vkNotifyRingMESA` when the ring is idle. Replies are written by the host into
  a reply stream set once with `vkSetReplyCommandStreamMESA`; each reply-
  generating command is preceded by `vkSeekReplyCommandStreamMESA(0)` so the
  reply lands at a known offset. The layout matches Mesa's `vn_ring`
  (`head`/`tail`/`status` 64-byte-aligned control words followed by a power-of-
  two ring buffer).
- **Real Vulkan object creation over the ring** — `VulkanSession` encodes and
  decodes real Vulkan commands against the host driver:
  `vkCreateInstance` (returns a `VkInstance`), `vkEnumeratePhysicalDevices`
  (two-call count/handles pattern), `vkGetPhysicalDeviceProperties` (decodes the
  device name, vendor/device IDs, type and API version), and `vkCreateDevice`
  (returns a `VkDevice` with one queue on family 0). Handles are guest-allocated
  object ids (8 bytes on the wire); teardown issues `vkDestroyDevice`,
  `vkDestroyInstance`, and `vkDestroyRingMESA`.
- **An end-to-end compute dispatch with CPU readback** — `VulkanSession::runCompute()`
  drives a full compute pipeline on the created device and verifies the result:
  - `vkGetPhysicalDeviceMemoryProperties` to pick a `HOST_VISIBLE | HOST_COHERENT`
    memory type;
  - `vkCreateBuffer` (storage) + `vkGetBufferMemoryRequirements` +
    `vkAllocateMemory` backed by a **HOST3D blob keyed by the memory object id**
    (`VirtIOGPUDriver::allocateContextBlobWithId`) which the guest maps for CPU
    access, then `vkBindBufferMemory`;
  - `vkCreateShaderModule` from an embedded SPIR-V compute shader
    (`data[i] = i*i + 1`, see `tools/venus-shader/`), a storage-buffer
    `vkCreateDescriptorSetLayout` / `vkCreateDescriptorPool` /
    `vkAllocateDescriptorSets` / `vkUpdateDescriptorSets`, `vkCreatePipelineLayout`
    and `vkCreateComputePipelines`;
  - `vkGetDeviceQueue2` (async, with the **required** `VkDeviceQueueTimelineInfoMESA`
    pNext, ringIdx 1), `vkCreateCommandPool` / `vkAllocateCommandBuffers`,
    recording `vkBeginCommandBuffer` + `vkCmdBindPipeline` +
    `vkCmdBindDescriptorSets` + `vkCmdDispatch` + `vkEndCommandBuffer`;
  - `vkQueueSubmit` signalling a `vkCreateFence`, then `vkWaitForFences`
    (the Venus renderer has no over-the-wire `vkQueueWaitIdle`; Mesa itself
    waits via a fence);
  - reading the mapped host-coherent memory back on the guest CPU and checking
    every element equals `i*i + 1`. All created objects are destroyed afterwards.
- **An on-screen graphics render** — `VulkanSession::renderTriangle()` runs a
  real graphics pipeline: it creates a `VK_FORMAT_B8G8R8A8_UNORM` color image +
  view, a render pass (clear → store, final layout `TRANSFER_SRC_OPTIMAL`), a
  framebuffer, vertex + fragment SPIR-V shaders (a gradient RGB triangle whose
  positions come from `gl_VertexIndex`, see `tools/venus-shader/triangle.*`), a
  graphics pipeline (full fixed-function state: input assembly, static viewport/
  scissor, rasterization, multisample, color blend), records `vkCmdBeginRenderPass`
  + `vkCmdBindPipeline` + `vkCmdDraw(3)` + `vkCmdEndRenderPass` +
  `vkCmdCopyImageToBuffer`, submits with a fence, and reads the rendered BGRA
  pixels back from a host-visible linear buffer. `Venus::renderTriangleToScreen()`
  then blits the result, centered, onto the virtio-gpu scanout and flushes it, so
  the GPU-rendered triangle is shown on the display.

All command opcodes, struct layouts and version constants are taken from Mesa's
generated `venus-protocol` headers (`vn_protocol_driver_*.h`) and the ring layout
from `vn_ring.c`.

## Boot self-test

At boot, after the virtio-gpu probe, `runVenusProbe()` in `src/main.cpp` runs the
full Venus bring-up and prints `[VENUS]` markers to the serial console, e.g.:

```
[VGPU] venus=yes
[VENUS] probe: ok
[VENUS] capset wire=1 vk_xml=40414e protocol=3
[VENUS] stages: capset=ok ctx=ok blob=ok submit=ok fence=ok reply=ok
[VENUS] vkEnumerateInstanceVersion=1.4.350 (raw=40415e)
[VENUS] response=OK_NODATA
[VENUS] vulkan: ok
[VENUS] vk stages: ring=ok instance=ok phys_devs=1 props=ok device=ok compute=ok
[VENUS] gpu0 name='llvmpipe (LLVM 22.1.5, 256 bits)' type=4 vendor=10005 api=1.4.348
[VENUS] instance=1001 device=1003
[VENUS] compute: elements=256 mismatches=0 data[3]=10 (expect 10)
[VENUS] triangle on screen: ok
```

If the host renderer is not in Venus mode (plain `virtio-gpu-pci`, or a QEMU/
virglrenderer without Venus), the probe prints `[VENUS] capset: unavailable` and
the system continues normally.

## Userspace syscalls

Userspace can drive Venus through two syscalls:

- **Probe** — `Syscall::GPUVenusProbeCall` → `sys_gpu_venus_probe`; ABI struct
  `GPUVenusProbe`; wrapper `gpu_venus_probe(GPUVenusProbe*)`. Reports
  availability and the synchronous `vkEnumerateInstanceVersion` round trip.
- **Vulkan bring-up** — `Syscall::GPUVenusVulkanCall` → `sys_gpu_venus_vulkan`;
  ABI struct `GPUVenusVulkan`; wrapper `gpu_venus_vulkan(GPUVenusVulkan*)`. Runs
  the full async-ring bring-up (instance + physical devices + properties +
  logical device) **and an end-to-end compute dispatch with CPU readback**, and
  returns the instance/device handles, device-0 properties, and the compute
  result (`computeOk`, `computeElements`, `computeMismatches`).

All structs live in `include/cpu/syscall/syscall.hpp` (mirrored in
`outside/iUserApps/outside/ilibcxx/include/syscall.hpp`).

```cpp
GPUVenusProbe probe = {};
if (gpu_venus_probe(&probe) == 0 && probe.available && probe.replyOk) {
    // probe.instanceVersion holds the host Vulkan instance version.
}

GPUVenusVulkan vk = {};
if (gpu_venus_vulkan(&vk) == 0 && vk.deviceOk) {
    // vk.deviceName / vk.vendorId / vk.apiVersion describe physical device 0;
    // vk.instanceHandle and vk.deviceHandle are the Venus object ids.
}
```

## Building and running

The Venus sources are picked up automatically by the CMake source glob, so a
normal build includes them:

```sh
cmake -S . -B build
cmake --build build --target iso --parallel 4
```

Venus requires a **Venus-capable** virtio-gpu device. The default `run.sh` uses
the plain `virtio-gpu-pci` (no host GL), so Venus is unavailable there. To enable
Venus, run with the GL device and a host-visible memory window:

```sh
qemu-system-x86_64 ... \
  -display egl-headless,gl=on \
  -device virtio-gpu-gl-pci,blob=on,venus=on,hostmem=256M
```

Host requirements:

- QEMU built with `virtio-gpu-gl` + Venus (the device must expose a `venus`
  property),
- `libvirglrenderer` built with `VK_MESA_venus_protocol`,
- a working host Vulkan ICD (a discrete-GPU driver such as `radv`, or the
  software `lavapipe`/`lvp` driver for deterministic CI).

## Watching the GPU triangle

To see the GPU-rendered triangle on screen, run with a **windowed, GL-capable**
display (the headless `egl-headless` path renders correctly but does not surface
the 2D scanout to `screendump`):

```sh
tools/run-venus-triangle.sh
# equivalently:
GPU_MODE=venus ./run.sh        # gtk,gl=on window with the Venus device
```

At boot the kernel renders a colored triangle with a real Vulkan graphics
pipeline, blits it centered onto the scanout, and holds it for a few seconds
(watch for `[VENUS] triangle on screen: ok` on the serial console). The pixels
are verified to reach the framebuffer in-kernel even in headless runs.

## Venus smoke test

`tools/run-venus-smoke.sh` builds the ISO, boots QEMU headless with
`virtio-gpu-gl-pci,blob=on,venus=on`, prefers the software Vulkan driver
(`lavapipe`) for reproducibility, and greps the serial log for the `[VENUS]`
markers:

```sh
bash tools/run-venus-smoke.sh
```

- Exits `0` and prints `venus smoke: PASS` when the full Venus round trip
  succeeds (`probe: ok` and `reply=ok`).
- Exits `0` and prints `venus smoke: SKIP` when the host QEMU/renderer lacks
  Venus support (so CI on non-Venus hosts is not a hard failure).
- Exits non-zero on a crash or an incomplete probe.

Useful overrides: `QEMU_ACCEL` (defaults to `kvm`), `QEMU_DISPLAY`
(defaults to `egl-headless,gl=on`), `HOSTMEM`, `VK_ICD_FILENAMES`.

## Debugging notes

- Host-side Venus errors are emitted by the QEMU render server on **stderr**
  (not the QEMU debug log file), e.g.
  `vkr: failed to set reply stream: invalid res_id N`. Capture stderr when
  diagnosing protocol issues.
- The Venus renderer processes the command stream asynchronously, so the
  submission fence completing does not guarantee the reply is written yet;
  `queryInstanceVersion()` polls the reply region (bounded) before decoding.
- The host render server initializes the Venus renderer lazily on the first
  context, which can fail on a cold start; the probe retries context creation a
  few times.

## Limitations

- The implemented Vulkan surface covers bring-up plus an end-to-end **compute**
  pipeline and a **graphics** render (offscreen triangle → image → buffer →
  scanout). Swapchains, depth/stencil, textures/samplers, and continuous
  per-frame rendering are not implemented. The CS encoder/decoder and async ring
  are general, so adding more commands is a matter of encoding their argument
  structs per the `venus-protocol` layout — no transport changes are needed.
- Ring submission is single-threaded and serializes each command (write → notify
  → wait for the host to consume → read reply). There is no batching or
  out-of-order completion yet; a higher-throughput path would pipeline multiple
  commands before reading replies.
- Reply delivery relies on a `vkSeekReplyCommandStreamMESA(0)` before each
  reply-generating command so the reply lands at a fixed offset. Commands that do
  not request a reply (e.g. destructors) are fire-and-forget on the ring.
- Object handles are simple guest-allocated monotonic ids; there is no handle
  table / lifetime tracking exposed to userspace beyond the returned ids.
