#pragma once

#include <stddef.h>
#include <stdint.h>

#include <graphics/virtio_gpu.hpp>

// Venus is Mesa's Vulkan-over-virtio-gpu protocol (VK_MESA_venus_protocol,
// capset id 4). This module implements the guest side of the Venus transport
// on top of the existing VirtIOGPUDriver 3D plumbing:
//
//   * capset 4 negotiation / version validation,
//   * a Venus-capset virtio-gpu context (CONTEXT_INIT with capset id 4),
//   * a guest-backed blob resource shared with the host renderer that holds the
//     command stream and the reply stream,
//   * a command-stream (CS) encoder/decoder matching the Venus wire format,
//   * a synchronous round trip that asks the host renderer for the Vulkan
//     instance version (vkEnumerateInstanceVersion) and reads the reply.
//
// The wire format is the stable little-endian encoding produced by Mesa's
// venus-protocol generator. Scalar widths on the wire:
//   uint32_t / VkFlags / VkStructureType / VkCommandTypeEXT  -> 4 bytes
//   uint64_t                                                 -> 8 bytes
//   size_t                                                   -> 8 bytes
//   "simple pointer" presence flag                           -> 4 bytes (0/1)
//   "array size" count                                       -> 8 bytes
// Every field advances the cursor by its size rounded up to 4 bytes; all of the
// types above are already 4-byte multiples, so no extra padding is inserted.

namespace venus {

// VkCommandTypeEXT opcodes used by the transport. Values are fixed by the
// Venus protocol (see vn_protocol_driver_defines.h, VK_MESA_venus_protocol).
enum VkCommandTypeEXT : uint32_t {
    VK_COMMAND_TYPE_vkCreateInstance = 0,
    VK_COMMAND_TYPE_vkDestroyInstance = 1,
    VK_COMMAND_TYPE_vkEnumeratePhysicalDevices = 2,
    VK_COMMAND_TYPE_vkGetPhysicalDeviceProperties = 6,
    VK_COMMAND_TYPE_vkGetPhysicalDeviceMemoryProperties = 8,
    VK_COMMAND_TYPE_vkCreateDevice = 11,
    VK_COMMAND_TYPE_vkDestroyDevice = 12,
    VK_COMMAND_TYPE_vkGetDeviceQueue = 17,
    VK_COMMAND_TYPE_vkQueueSubmit = 18,
    VK_COMMAND_TYPE_vkQueueWaitIdle = 19,
    VK_COMMAND_TYPE_vkAllocateMemory = 21,
    VK_COMMAND_TYPE_vkFreeMemory = 22,
    VK_COMMAND_TYPE_vkBindBufferMemory = 28,
    VK_COMMAND_TYPE_vkBindImageMemory = 29,
    VK_COMMAND_TYPE_vkGetBufferMemoryRequirements = 30,
    VK_COMMAND_TYPE_vkGetImageMemoryRequirements = 31,
    VK_COMMAND_TYPE_vkCreateFence = 35,
    VK_COMMAND_TYPE_vkDestroyFence = 36,
    VK_COMMAND_TYPE_vkWaitForFences = 39,
    VK_COMMAND_TYPE_vkCreateBuffer = 50,
    VK_COMMAND_TYPE_vkDestroyBuffer = 51,
    VK_COMMAND_TYPE_vkCreateImage = 54,
    VK_COMMAND_TYPE_vkDestroyImage = 55,
    VK_COMMAND_TYPE_vkCreateImageView = 57,
    VK_COMMAND_TYPE_vkDestroyImageView = 58,
    VK_COMMAND_TYPE_vkCreateShaderModule = 59,
    VK_COMMAND_TYPE_vkDestroyShaderModule = 60,
    VK_COMMAND_TYPE_vkCreateGraphicsPipelines = 65,
    VK_COMMAND_TYPE_vkCreateComputePipelines = 66,
    VK_COMMAND_TYPE_vkDestroyPipeline = 67,
    VK_COMMAND_TYPE_vkCreatePipelineLayout = 68,
    VK_COMMAND_TYPE_vkDestroyPipelineLayout = 69,
    VK_COMMAND_TYPE_vkCreateDescriptorSetLayout = 72,
    VK_COMMAND_TYPE_vkDestroyDescriptorSetLayout = 73,
    VK_COMMAND_TYPE_vkCreateDescriptorPool = 74,
    VK_COMMAND_TYPE_vkDestroyDescriptorPool = 75,
    VK_COMMAND_TYPE_vkAllocateDescriptorSets = 77,
    VK_COMMAND_TYPE_vkUpdateDescriptorSets = 79,
    VK_COMMAND_TYPE_vkCreateFramebuffer = 80,
    VK_COMMAND_TYPE_vkDestroyFramebuffer = 81,
    VK_COMMAND_TYPE_vkCreateRenderPass = 82,
    VK_COMMAND_TYPE_vkDestroyRenderPass = 83,
    VK_COMMAND_TYPE_vkCreateCommandPool = 85,
    VK_COMMAND_TYPE_vkDestroyCommandPool = 86,
    VK_COMMAND_TYPE_vkAllocateCommandBuffers = 88,
    VK_COMMAND_TYPE_vkBeginCommandBuffer = 90,
    VK_COMMAND_TYPE_vkEndCommandBuffer = 91,
    VK_COMMAND_TYPE_vkCmdBindPipeline = 93,
    VK_COMMAND_TYPE_vkCmdBindDescriptorSets = 103,
    VK_COMMAND_TYPE_vkCmdDraw = 106,
    VK_COMMAND_TYPE_vkCmdDispatch = 110,
    VK_COMMAND_TYPE_vkCmdCopyImageToBuffer = 116,
    VK_COMMAND_TYPE_vkCmdPipelineBarrier = 126,
    VK_COMMAND_TYPE_vkCmdBeginRenderPass = 133,
    VK_COMMAND_TYPE_vkCmdEndRenderPass = 135,
    VK_COMMAND_TYPE_vkEnumerateInstanceVersion = 137,
    VK_COMMAND_TYPE_vkGetDeviceQueue2 = 155,
    VK_COMMAND_TYPE_vkSetReplyCommandStreamMESA = 178,
    VK_COMMAND_TYPE_vkSeekReplyCommandStreamMESA = 179,
    VK_COMMAND_TYPE_vkExecuteCommandStreamsMESA = 180,
    VK_COMMAND_TYPE_vkCreateRingMESA = 188,
    VK_COMMAND_TYPE_vkDestroyRingMESA = 189,
    VK_COMMAND_TYPE_vkNotifyRingMESA = 190,
};

// VkStructureType values used in the encoded structs.
constexpr uint32_t VK_STRUCTURE_TYPE_APPLICATION_INFO = 0;
constexpr uint32_t VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1;
constexpr uint32_t VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2;
constexpr uint32_t VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3;
constexpr uint32_t VK_STRUCTURE_TYPE_SUBMIT_INFO = 4;
constexpr uint32_t VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5;
constexpr uint32_t VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8;
constexpr uint32_t VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12;
constexpr uint32_t VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16;
constexpr uint32_t VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18;
constexpr uint32_t VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO = 29;
constexpr uint32_t VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30;
constexpr uint32_t VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32;
constexpr uint32_t VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33;
constexpr uint32_t VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34;
constexpr uint32_t VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35;
constexpr uint32_t VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39;
constexpr uint32_t VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40;
constexpr uint32_t VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42;
constexpr uint32_t VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2 = 1000145003;
constexpr uint32_t VK_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA = 1000384005;
// Graphics pipeline / render pass structure types.
constexpr uint32_t VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14;
constexpr uint32_t VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15;
constexpr uint32_t VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19;
constexpr uint32_t VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20;
constexpr uint32_t VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22;
constexpr uint32_t VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23;
constexpr uint32_t VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24;
constexpr uint32_t VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26;
constexpr uint32_t VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28;
constexpr uint32_t VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37;
constexpr uint32_t VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38;
constexpr uint32_t VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43;

// Enum / flag values used by the graphics path.
constexpr uint32_t VK_FORMAT_B8G8R8A8_UNORM = 44;
constexpr uint32_t VK_IMAGE_TYPE_2D = 1;
constexpr uint32_t VK_IMAGE_VIEW_TYPE_2D = 1;
constexpr uint32_t VK_IMAGE_TILING_OPTIMAL = 0;
constexpr uint32_t VK_IMAGE_LAYOUT_UNDEFINED = 0;
constexpr uint32_t VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2;
constexpr uint32_t VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6;
constexpr uint32_t VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x00000001;
constexpr uint32_t VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x00000010;
constexpr uint32_t VK_IMAGE_ASPECT_COLOR_BIT = 0x00000001;
constexpr uint32_t VK_SAMPLE_COUNT_1_BIT = 0x00000001;
constexpr uint32_t VK_COMPONENT_SWIZZLE_IDENTITY = 0;
constexpr uint32_t VK_ATTACHMENT_LOAD_OP_CLEAR = 1;
constexpr uint32_t VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2;
constexpr uint32_t VK_ATTACHMENT_STORE_OP_STORE = 0;
constexpr uint32_t VK_ATTACHMENT_STORE_OP_DONT_CARE = 1;
constexpr uint32_t VK_PIPELINE_BIND_POINT_GRAPHICS = 0;
constexpr uint32_t VK_SUBPASS_CONTENTS_INLINE = 0;
constexpr uint32_t VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3;
constexpr uint32_t VK_POLYGON_MODE_FILL = 0;
constexpr uint32_t VK_CULL_MODE_NONE = 0;
constexpr uint32_t VK_FRONT_FACE_COUNTER_CLOCKWISE = 0;
constexpr uint32_t VK_SHADER_STAGE_VERTEX_BIT = 0x00000001;
constexpr uint32_t VK_SHADER_STAGE_FRAGMENT_BIT = 0x00000010;
constexpr uint32_t VK_COLOR_COMPONENT_RGBA = 0x0000000F;
constexpr uint32_t VK_LOGIC_OP_CLEAR = 0;
constexpr uint32_t VK_BLEND_FACTOR_ONE = 1;
constexpr uint32_t VK_BLEND_FACTOR_ZERO = 0;
constexpr uint32_t VK_BLEND_OP_ADD = 0;
// IEEE-754 bit patterns (the freestanding kernel avoids the soft-float runtime).
constexpr uint32_t kFloatZeroBits = 0x00000000u;
constexpr uint32_t kFloatOneBits = 0x3F800000u;

// Enum / flag values used by the compute path.
constexpr uint32_t VK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7;
constexpr uint32_t VK_SHADER_STAGE_COMPUTE_BIT = 0x00000020;
constexpr uint32_t VK_PIPELINE_BIND_POINT_COMPUTE = 1;
constexpr uint32_t VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 0x00000020;
constexpr uint32_t VK_SHARING_MODE_EXCLUSIVE = 0;
constexpr uint32_t VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0;
constexpr uint32_t VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x00000001;
constexpr uint32_t VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x00000002;
constexpr uint32_t VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x00000004;
constexpr uint32_t VK_MAX_MEMORY_TYPES = 32;
constexpr uint32_t VK_MAX_MEMORY_HEAPS = 16;
constexpr uint64_t VK_WHOLE_SIZE = ~0ULL;

// Vulkan limits used when decoding VkPhysicalDeviceProperties.
constexpr uint32_t VK_MAX_PHYSICAL_DEVICE_NAME_SIZE = 256;
constexpr uint32_t VK_UUID_SIZE = 16;

// VkCommandFlagBitsEXT.
constexpr uint32_t VK_COMMAND_GENERATE_REPLY_BIT = 0x00000001U;

// VkStructureType for the ring create info pNext-less chain.
constexpr uint32_t VK_STRUCTURE_TYPE_RING_CREATE_INFO_MESA = 1000384000U;

// VkRingStatusFlagBitsMESA.
constexpr uint32_t VK_RING_STATUS_IDLE_BIT = 0x00000001U;
constexpr uint32_t VK_RING_STATUS_FATAL_BIT = 0x00000002U;
constexpr uint32_t VK_RING_STATUS_ALIVE_BIT = 0x00000004U;

// Expected Venus capset contents (the guest validates against these).
constexpr uint32_t kVenusWireFormatVersion = 1;
// VK_MAKE_API_VERSION(0, 1, 1, 0): the lowest Vulkan version we require the
// host to advertise. virglrenderer reports a much higher version; we only need
// a sane non-zero floor.
constexpr uint32_t kVenusMinVkXmlVersion = (1U << 22) | (1U << 12);

// A bounded, allocation-free command-stream encoder. It writes the Venus wire
// format into a caller-supplied buffer and never writes past it.
class CsEncoder {
public:
    CsEncoder(void* buffer, size_t capacity)
        : buffer_(static_cast<uint8_t*>(buffer)), capacity_(capacity), cursor_(0), overflow_(false) {}

    size_t length() const { return cursor_; }
    bool overflowed() const { return overflow_; }
    void reset() { cursor_ = 0; overflow_ = false; }

    void writeU32(uint32_t value);
    void writeU64(uint64_t value);
    // size_t is serialized as 8 bytes on the wire.
    void writeSizeT(uint64_t value) { writeU64(value); }
    void writeFlags(uint32_t value) { writeU32(value); }
    void writeCommandType(uint32_t value) { writeU32(value); }
    void writeStructureType(uint32_t value) { writeU32(value); }
    // Encodes a "simple pointer" presence flag. In the Venus wire format this
    // is an 8-byte value (vn_sizeof_simple_pointer == vn_sizeof_array_size ==
    // vn_sizeof_uint64_t). Returns whether the pointee should follow.
    bool writeSimplePointer(bool present);
    // Encodes an "array size" count (8 bytes).
    void writeArraySize(uint64_t count) { writeU64(count); }
    // Encodes a Vulkan handle (object id): an 8-byte value.
    void writeHandle(uint64_t id) { writeU64(id); }
    // Encodes a 32-bit float given its IEEE-754 bit pattern (4 bytes). We pass
    // bits rather than a float to keep the freestanding kernel free of the
    // soft-float runtime (_fltused).
    void writeFloatBits(uint32_t bits) { writeU32(bits); }
    // Encodes a raw blob: writes `size` bytes then pads the cursor up to a
    // 4-byte multiple (vn_encode_blob_array semantics).
    void writeBlob(const void* data, size_t size);
    // Encodes a NUL-terminated string as Venus does: an 8-byte array size
    // (strlen+1) followed by the 4-aligned character blob.
    void writeString(const char* str);

private:
    bool reserve(size_t bytes);

    uint8_t* buffer_;
    size_t capacity_;
    size_t cursor_;
    bool overflow_;
};

// A bounded command-stream decoder for reading replies.
class CsDecoder {
public:
    CsDecoder(const void* buffer, size_t size)
        : buffer_(static_cast<const uint8_t*>(buffer)), size_(size), cursor_(0), fatal_(false) {}

    bool fatal() const { return fatal_; }
    size_t remaining() const { return cursor_ <= size_ ? size_ - cursor_ : 0; }

    uint32_t readU32();
    uint64_t readU64();
    // "simple pointer" / "array size" presence values are 8 bytes on the wire.
    uint64_t readArraySize() { return readU64(); }
    uint64_t readHandle() { return readU64(); }
    // Reads a 4-aligned blob of `size` bytes into `out` (advancing the 4-padded
    // amount). Truncates the copy to `out` if `outCap` is smaller.
    void readBlob(void* out, size_t size, size_t outCap);
    // Skips `bytes` rounded up to a 4-byte multiple.
    void skipAligned(size_t bytes);

private:
    bool require(size_t bytes);

    const uint8_t* buffer_;
    size_t size_;
    size_t cursor_;
    bool fatal_;
};

// Result of bringing up Venus and performing the instance-version round trip.
struct VenusProbeResult {
    bool capsetOk;            // capset 4 found and parsed
    bool capsetVersionOk;     // wire format / vk.xml version acceptable
    bool contextOk;           // Venus-capset context created
    bool blobOk;              // shared command/reply blob allocated + attached
    bool submitOk;            // SUBMIT_3D transport succeeded
    bool fenceOk;             // submission fence completed
    bool replyOk;             // a well-formed reply was read back
    uint32_t capsetVersion;
    uint32_t wireFormatVersion;
    uint32_t vkXmlVersion;
    uint32_t venusProtocolVersion;
    uint32_t instanceVersion; // vkEnumerateInstanceVersion result (if replyOk)
    uint32_t ctxId;
    uint32_t resourceId;
    uint32_t responseType;    // virtio-gpu response type of the submit
};

// Decoded VkPhysicalDeviceProperties (the subset we surface to callers).
struct VulkanPhysicalDeviceInfo {
    uint64_t handle;          // Venus object id for the physical device
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorId;
    uint32_t deviceId;
    uint32_t deviceType;      // VkPhysicalDeviceType
    char deviceName[256];
};

// Maximum physical devices we enumerate / track per session.
constexpr uint32_t kVenusMaxPhysicalDevices = 8;

// A live Vulkan session over Venus: owns a virtio-gpu Venus context, a shared
// command/reply blob, and an async command ring. It encodes real Vulkan
// commands (vkCreateInstance, vkEnumeratePhysicalDevices,
// vkGetPhysicalDeviceProperties, vkCreateDevice) through the ring and decodes
// their replies.
class VulkanSession {
public:
    VulkanSession() = default;
    ~VulkanSession();

    VulkanSession(const VulkanSession&) = delete;
    VulkanSession& operator=(const VulkanSession&) = delete;

    // Brings up the context + ring. Must be called before any command.
    bool init();
    bool isReady() const { return ringReady_; }

    // vkCreateInstance with apiVersion 1.1 and no layers/extensions. On success
    // stores the instance handle and returns VK_SUCCESS (0).
    int createInstance(uint32_t apiVersion = (1U << 22) | (1U << 12));
    uint64_t instanceHandle() const { return instance_; }

    // vkEnumeratePhysicalDevices: queries the count then the handles. Fills the
    // internal table; returns the number found (0 on failure).
    uint32_t enumeratePhysicalDevices();
    uint32_t physicalDeviceCount() const { return physDevCount_; }

    // vkGetPhysicalDeviceProperties for a previously enumerated device index.
    bool getPhysicalDeviceProperties(uint32_t index, VulkanPhysicalDeviceInfo* outInfo);

    // vkCreateDevice on a physical device (single graphics-agnostic queue,
    // family 0). On success stores the device handle and returns VK_SUCCESS.
    int createDevice(uint32_t physicalDeviceIndex);
    uint64_t deviceHandle() const { return device_; }

    // Runs an end-to-end compute dispatch on a created device: allocates a
    // host-visible storage buffer (backed by a HOST3D blob the guest maps),
    // creates a SPIR-V compute shader + descriptor set + compute pipeline,
    // records a command buffer (bind pipeline + descriptors, dispatch),
    // submits to the device queue, waits for idle, and reads back the result.
    // The bundled shader computes data[i] = i*i + 1 for `elementCount` elements.
    // On success writes up to `outCap` result words into `out` and returns true
    // iff every readback word matched the expected value.
    bool runCompute(uint32_t elementCount, uint32_t* out, uint32_t outCap,
                    uint32_t* outMismatches = nullptr);

    // Renders a colored triangle with a real graphics pipeline into an offscreen
    // VK_FORMAT_B8G8R8A8_UNORM image of size width x height, then copies the
    // image into a host-visible buffer and writes the BGRA pixels into `outBGRA`
    // (a width*height uint32 array, one 0xAARRGGBB-ish BGRA word per pixel).
    // Returns true if the whole pipeline ran and the readback is non-empty.
    bool renderTriangle(uint32_t width, uint32_t height, uint32_t* outBGRA, uint32_t outCapWords);

    // Tear down device, instance, ring, and context.
    void shutdown();

private:
    // Allocates a fresh Venus object id for a guest-created handle.
    uint64_t allocHandleId() { return ++nextHandleId_; }

    // Submits an encoded command stream through the ring. If replySize > 0,
    // the host writes a reply into the reply region; the call blocks (bounded)
    // until the ring consumes the command and the reply is visible, then points
    // *outReply at the reply bytes. Returns false on transport/timeout error.
    bool ringSubmit(const void* cmd, uint32_t cmdSize, uint32_t replySize,
                    const uint8_t** outReply);

    // Sets the per-ring reply command stream (region of our blob the host
    // writes replies into).
    bool setReplyStream();

    // Queries vkGetPhysicalDeviceMemoryProperties and selects a memory type
    // index that is HOST_VISIBLE | HOST_COHERENT (and in `memoryTypeBits`).
    // Returns false if none is suitable.
    bool selectHostVisibleMemoryType(uint64_t physDev, uint32_t memoryTypeBits,
                                     uint32_t* outTypeIndex);

    // vkAllocateMemory of `size` from `memoryTypeIndex`, then back it with a
    // HOST3D blob keyed by the memory's object id and map it into the guest.
    // Writes the device-memory handle, the blob resource id, and a CPU pointer.
    bool allocateMappableMemory(uint64_t size, uint32_t memoryTypeIndex,
                                uint64_t* outMemory, uint32_t* outResId, void** outPtr);

    // Submits an already-encoded `vkCreateX`-style command (one that returns a
    // VkResult + a single object handle) and returns its VkResult (negative on
    // transport/decode failure). `cmd`/`cmdSize` is the staged stream.
    int decodeCreateReply(const void* cmd, uint64_t cmdSize, uint32_t expectedCmdType);
    // Like decodeCreateReply but for commands whose reply is just a VkResult
    // (no trailing handle), e.g. vkBindBufferMemory / vkQueueSubmit / Begin/End.
    int decodeStatusReply(const void* cmd, uint64_t cmdSize, uint32_t expectedCmdType);

    VirtIOGPUDriver* gpu_ = nullptr;
    uint32_t ctxId_ = 0;
    uint32_t resourceId_ = 0;
    uint8_t* blob_ = nullptr;
    uint64_t blobLength_ = 0;

    // Ring geometry within the blob (see init()).
    uint64_t ringId_ = 0;
    uint64_t headOffset_ = 0;
    uint64_t tailOffset_ = 0;
    uint64_t statusOffset_ = 0;
    uint64_t bufferOffset_ = 0;
    uint64_t bufferSize_ = 0;     // power of two
    uint64_t replyOffset_ = 0;
    uint64_t replySize_ = 0;
    uint64_t scratchOffset_ = 0;  // staging area for building a command stream
    uint64_t scratchSize_ = 0;
    uint64_t ringCur_ = 0;        // monotonic tail (bytes written)
    bool ringReady_ = false;
    bool replyStreamSet_ = false;

    uint64_t nextHandleId_ = 0x1000;
    uint64_t instance_ = 0;
    uint64_t device_ = 0;
    uint32_t physDevCount_ = 0;
    uint64_t physDevs_[kVenusMaxPhysicalDevices] = {};
};

// Result of the fuller Vulkan bring-up (instance + device) for diagnostics and
// the syscall surface.
struct VenusVulkanResult {
    bool ringOk;              // async ring created
    bool instanceOk;         // vkCreateInstance succeeded
    bool physDevOk;          // at least one physical device enumerated
    bool propsOk;            // properties read for device 0
    bool deviceOk;           // vkCreateDevice succeeded
    bool computeOk;          // compute dispatch ran and all readback words matched
    uint32_t physDevCount;
    uint32_t computeElements; // number of elements dispatched/verified
    uint32_t computeMismatches;
    uint32_t computeSample;   // result[3] (expected 3*3+1 = 10) for a quick sanity print
    uint64_t instanceHandle;
    uint64_t deviceHandle;
    VulkanPhysicalDeviceInfo device0;
};

// The Venus driver. It is a thin, stateful layer over VirtIOGPUDriver and is
// accessed as a singleton, mirroring the rest of the graphics stack.
class Venus {
public:
    static Venus& get();

    // Returns true if the host advertises a compatible Venus capset.
    bool isAvailable() const { return available_; }
    uint32_t capsetVersion() const { return capset_.vk_mesa_venus_protocol_spec_version; }

    // Negotiates the Venus capset (id 4) and validates its version. Safe to
    // call multiple times; caches the result. Returns true on success.
    bool negotiate();

    // Brings up a Venus context with a shared command/reply blob, performs a
    // synchronous vkEnumerateInstanceVersion round trip against the host
    // renderer, fills *result, and tears everything down. Returns true only if
    // the full path (context + blob + submit + fence + reply) succeeded.
    bool probe(VenusProbeResult* result = nullptr);

    // Brings up a full Vulkan session over the async ring: creates an instance,
    // enumerates physical devices, reads device 0 properties, and creates a
    // logical device. Fills *result and tears everything down. Returns true
    // only if instance + physical device + device creation all succeeded.
    bool bringUpVulkan(VenusVulkanResult* result = nullptr);

    // Renders a GPU triangle (graphics pipeline) into an offscreen image and
    // blits the result, centered, onto the virtio-gpu framebuffer + flushes it,
    // so the rendered image is visible on the display. Returns true if the GPU
    // render + present succeeded. `size` is the square render target edge.
    bool renderTriangleToScreen(uint32_t size = 480);

    // Lower-level building blocks (also used by the syscall surface):

    // Creates a Venus context. Writes the context id to *outCtxId.
    bool createContext(uint32_t* outCtxId, const char* debugName = "venus");
    // Destroys a context previously created with createContext.
    void destroyContext(uint32_t ctxId);

    // Allocates a host-allocated, mapped blob shared with the renderer and
    // attaches it to the given context. Returns the resource id and a guest
    // pointer to the (zeroed) memory.
    bool allocateSharedBlob(uint32_t ctxId, uint64_t size, uint32_t* outResourceId,
                            void** outPtr, uint64_t* outLength);

    // Encodes the standard Venus bring-up + vkEnumerateInstanceVersion request
    // into the shared blob and submits it, then decodes the reply. The blob is
    // laid out as: [request CS][reply region]. Returns true on a valid reply
    // and writes the Vulkan instance version to *outInstanceVersion.
    bool queryInstanceVersion(uint32_t ctxId, uint32_t resourceId, void* blob, uint64_t blobLength,
                              uint32_t* outInstanceVersion, uint32_t* outResponseType);

private:
    Venus() = default;

    bool available_ = false;
    bool negotiated_ = false;
    VirtIOGPUVenusCapset capset_ = {};
};

} // namespace venus
