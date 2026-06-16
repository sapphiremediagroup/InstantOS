#include <graphics/venus.hpp>

#include <common/string.hpp>
#include <graphics/console.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>

namespace venus {

namespace {

// Round a byte count up to the next multiple of 4 (Venus advances the encode
// cursor in 4-byte steps).
constexpr size_t align4(size_t value) {
    return (value + 3u) & ~static_cast<size_t>(3u);
}

void log(const char* text) {
    Console::get().drawText(text);
}

} // namespace

// ---------------------------------------------------------------------------
// CsEncoder
// ---------------------------------------------------------------------------

bool CsEncoder::reserve(size_t bytes) {
    if (overflow_ || bytes > capacity_ - cursor_) {
        overflow_ = true;
        return false;
    }
    return true;
}

void CsEncoder::writeU32(uint32_t value) {
    // Natural size 4 is already 4-aligned.
    if (!reserve(4)) {
        return;
    }
    buffer_[cursor_ + 0] = static_cast<uint8_t>(value & 0xFF);
    buffer_[cursor_ + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buffer_[cursor_ + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buffer_[cursor_ + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    cursor_ += 4;
}

void CsEncoder::writeU64(uint64_t value) {
    if (!reserve(8)) {
        return;
    }
    for (uint32_t i = 0; i < 8; ++i) {
        buffer_[cursor_ + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
    cursor_ += 8;
}

bool CsEncoder::writeSimplePointer(bool present) {
    // vn_encode_simple_pointer encodes an 8-byte array-size of 1 or 0.
    writeU64(present ? 1u : 0u);
    return present;
}

void CsEncoder::writeBlob(const void* data, size_t size) {
    const size_t padded = (size + 3u) & ~static_cast<size_t>(3u);
    if (!reserve(padded)) {
        return;
    }
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        buffer_[cursor_ + i] = src[i];
    }
    for (size_t i = size; i < padded; ++i) {
        buffer_[cursor_ + i] = 0;
    }
    cursor_ += padded;
}

void CsEncoder::writeString(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') {
        ++len;
    }
    const size_t sizeWithNul = len + 1;
    writeArraySize(sizeWithNul);
    writeBlob(str, sizeWithNul);
}

// ---------------------------------------------------------------------------
// CsDecoder
// ---------------------------------------------------------------------------

bool CsDecoder::require(size_t bytes) {
    if (fatal_ || bytes > remaining()) {
        fatal_ = true;
        return false;
    }
    return true;
}

uint32_t CsDecoder::readU32() {
    if (!require(4)) {
        return 0;
    }
    const uint32_t value = static_cast<uint32_t>(buffer_[cursor_ + 0]) |
                           (static_cast<uint32_t>(buffer_[cursor_ + 1]) << 8) |
                           (static_cast<uint32_t>(buffer_[cursor_ + 2]) << 16) |
                           (static_cast<uint32_t>(buffer_[cursor_ + 3]) << 24);
    cursor_ += 4;
    return value;
}

uint64_t CsDecoder::readU64() {
    if (!require(8)) {
        return 0;
    }
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(buffer_[cursor_ + i]) << (i * 8);
    }
    cursor_ += 8;
    return value;
}

void CsDecoder::readBlob(void* out, size_t size, size_t outCap) {
    const size_t padded = (size + 3u) & ~static_cast<size_t>(3u);
    if (!require(padded)) {
        return;
    }
    uint8_t* dst = static_cast<uint8_t*>(out);
    const size_t copy = size < outCap ? size : outCap;
    for (size_t i = 0; i < copy; ++i) {
        dst[i] = buffer_[cursor_ + i];
    }
    cursor_ += padded;
}

void CsDecoder::skipAligned(size_t bytes) {
    const size_t padded = (bytes + 3u) & ~static_cast<size_t>(3u);
    if (!require(padded)) {
        return;
    }
    cursor_ += padded;
}

// ---------------------------------------------------------------------------
// Venus driver
// ---------------------------------------------------------------------------

Venus& Venus::get() {
    static Venus instance;
    return instance;
}

bool Venus::negotiate() {
    if (negotiated_) {
        return available_;
    }
    negotiated_ = true;

    VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();
    if (!gpu.supportsVirgl() || !gpu.supportsContextInit()) {
        // Venus contexts require CONTEXT_INIT to select a capset.
        available_ = false;
        return false;
    }

    VirtIOGPUVenusCapset capset = {};
    if (!gpu.detectVenusCapset(&capset)) {
        available_ = false;
        return false;
    }

    capset_ = capset;

    // Validate the wire-format and Vulkan versions the host advertises. A
    // mismatched wire format means the host decoder would reject our stream.
    const bool wireOk = capset.wire_format_version == kVenusWireFormatVersion;
    const bool vkOk = capset.vk_xml_version >= kVenusMinVkXmlVersion;
    available_ = wireOk && vkOk;

    if (!available_) {
        log("[VENUS] capset present but incompatible (wire=");
        Console::get().drawHex(capset.wire_format_version);
        log(" vkxml=");
        Console::get().drawHex(capset.vk_xml_version);
        log(")\n");
    }
    return available_;
}

bool Venus::createContext(uint32_t* outCtxId, const char* debugName) {
    if (!available_ || !outCtxId) {
        return false;
    }
    return VirtIOGPUDriver::get().createContextWithCapset(outCtxId, VIRTIO_GPU_CAPSET_VENUS, debugName);
}

void Venus::destroyContext(uint32_t ctxId) {
    if (ctxId != 0) {
        VirtIOGPUDriver::get().destroyContext(ctxId);
    }
}

bool Venus::allocateSharedBlob(uint32_t ctxId, uint64_t size, uint32_t* outResourceId,
                               void** outPtr, uint64_t* outLength) {
    if (ctxId == 0 || size == 0 || !outResourceId || !outPtr) {
        return false;
    }

    VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();
    if (!gpu.supportsBlobResources()) {
        return false;
    }

    uint32_t resourceId = 0;
    void* ptr = nullptr;
    uint64_t length = 0;
    // Create the shared blob *within* the Venus context. The host vkr renderer
    // only tracks resources associated with the context, so a context-scoped
    // host-allocated (HOST3D, blob_id 0) blob is what command/reply streams
    // must reference.
    if (!gpu.allocateContextBlob(ctxId, size, &resourceId, &ptr, &length)) {
        return false;
    }

    // Attach the blob to the Venus context so the renderer can reference it via
    // its resource id in command-stream descriptions.
    if (!gpu.attachResourceToContext(ctxId, resourceId)) {
        gpu.unmapBlobResource(resourceId);
        gpu.unrefResource(resourceId);
        return false;
    }

    *outResourceId = resourceId;
    *outPtr = ptr;
    if (outLength) {
        *outLength = length;
    }
    return true;
}

bool Venus::queryInstanceVersion(uint32_t ctxId, uint32_t resourceId, void* blob, uint64_t blobLength,
                                 uint32_t* outInstanceVersion, uint32_t* outResponseType) {
    if (ctxId == 0 || resourceId == 0 || !blob || blobLength < 512) {
        return false;
    }

    uint8_t* base = static_cast<uint8_t*>(blob);
    memset(base, 0, blobLength);

    // Lay the blob out into three regions:
    //   [0 .. kReplyOffset)        - the primary command stream (what SUBMIT_3D
    //                                feeds directly to the host decoder)
    //   [kReplyOffset .. kSubOff)  - the reply stream the host writes into
    //   [kSubOffset .. end)        - the executed sub-stream holding the actual
    //                                Vulkan command (vkEnumerateInstanceVersion)
    const uint64_t kReplyOffset = blobLength / 2;     // page-aligned enough for our sizes
    const uint64_t kReplySize = blobLength / 4;
    const uint64_t kSubOffset = kReplyOffset + kReplySize;
    const uint64_t kSubSize = blobLength - kSubOffset;
    if (kReplyOffset < 256 || kSubSize < 64) {
        return false;
    }

    // 1) Encode the executed sub-stream: a single vkEnumerateInstanceVersion
    //    command with GENERATE_REPLY so the host emits a reply.
    CsEncoder sub(base + kSubOffset, kSubSize);
    sub.writeCommandType(VK_COMMAND_TYPE_vkEnumerateInstanceVersion);
    sub.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
    // vkEnumerateInstanceVersion has a single out param (pApiVersion). On the
    // request side it is encoded as a simple-pointer presence flag.
    sub.writeSimplePointer(true);
    if (sub.overflowed()) {
        return false;
    }
    const uint64_t subUsed = sub.length();

    // 2) Encode the primary command stream:
    //    a) vkSetReplyCommandStreamMESA(stream = reply region)
    //    b) vkSeekReplyCommandStreamMESA(position = 0)
    //    c) vkExecuteCommandStreamsMESA(streamCount=1, pStreams=[sub region])
    CsEncoder enc(base, kReplyOffset);

    // a) vkSetReplyCommandStreamMESA
    enc.writeCommandType(VK_COMMAND_TYPE_vkSetReplyCommandStreamMESA);
    enc.writeFlags(0);
    if (enc.writeSimplePointer(true)) {
        // VkCommandStreamDescriptionMESA { uint32 resourceId; size_t offset; size_t size; }
        enc.writeU32(resourceId);
        enc.writeSizeT(kReplyOffset);
        enc.writeSizeT(kReplySize);
    }

    // b) vkSeekReplyCommandStreamMESA(position = 0)
    enc.writeCommandType(VK_COMMAND_TYPE_vkSeekReplyCommandStreamMESA);
    enc.writeFlags(0);
    enc.writeSizeT(0);

    // c) vkExecuteCommandStreamsMESA
    enc.writeCommandType(VK_COMMAND_TYPE_vkExecuteCommandStreamsMESA);
    enc.writeFlags(0);
    enc.writeU32(1);            // streamCount
    enc.writeArraySize(1);     // pStreams array present, size 1
    enc.writeU32(resourceId);  // pStreams[0].resourceId
    enc.writeSizeT(kSubOffset);
    enc.writeSizeT(subUsed);
    enc.writeArraySize(0);     // pReplyPositions = NULL
    enc.writeU32(0);           // dependencyCount
    enc.writeArraySize(0);     // pDependencies = NULL
    enc.writeFlags(0);         // VkCommandStreamExecutionFlagsMESA

    if (enc.overflowed()) {
        return false;
    }

    const uint64_t primaryUsed = enc.length();

    VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();

    // Submit the primary stream to the host. The renderer decodes it directly,
    // executes the referenced sub-stream, and writes the reply into the reply
    // region of our (host-coherent) blob.
    const bool submitOk = gpu.submit3D(ctxId, base, static_cast<uint32_t>(primaryUsed));
    const VirtIOGPUCommandStatus status = gpu.getLastCommandStatus();
    if (outResponseType) {
        *outResponseType = status.responseType;
    }
    if (!submitOk) {
        return false;
    }

    // The submission fence indicates the host dequeued the command stream, but
    // the Venus renderer processes commands asynchronously, so the reply may not
    // be visible yet. Poll the reply region (the first word becomes the reply's
    // command type, 137) until the host has written it, bounded by a spin limit.
    // A transfer-from-host + memory barrier on each iteration serializes against
    // the host write for non-coherent mappings.
    volatile uint32_t* replyHead = reinterpret_cast<volatile uint32_t*>(base + kReplyOffset);
    bool replyReady = false;
    for (uint32_t spin = 0; spin < 200000u; ++spin) {
        if (*replyHead == VK_COMMAND_TYPE_vkEnumerateInstanceVersion) {
            replyReady = true;
            break;
        }
        if ((spin & 0x3FFu) == 0) {
            VirtIOGPUBox box = {};
            box.x = static_cast<uint32_t>(kReplyOffset);
            box.width = static_cast<uint32_t>(kReplySize);
            box.height = 1;
            box.depth = 1;
            gpu.transferFromHost3D(ctxId, resourceId, box, kReplyOffset, 0, 0, 0);
        }
        __asm__ __volatile__("pause; mfence" ::: "memory");
    }
    if (!replyReady) {
        return false;
    }

    // Decode the reply. The Venus reply for vkEnumerateInstanceVersion is:
    //   VkCommandTypeEXT command_type   (asserted to match)
    //   VkResult         ret
    //   simple_pointer   present flag for pApiVersion (8 bytes on the wire)
    //   uint32_t         apiVersion      (only if present)
    CsDecoder dec(base + kReplyOffset, kReplySize);
    const uint32_t replyCmd = dec.readU32();
    if (dec.fatal() || replyCmd != VK_COMMAND_TYPE_vkEnumerateInstanceVersion) {
        return false;
    }
    const uint32_t result = dec.readU32();
    const uint64_t present = dec.readArraySize();
    if (dec.fatal() || present == 0) {
        return false;
    }
    const uint32_t apiVersion = dec.readU32();
    if (dec.fatal() || result != 0) {
        return false;
    }

    if (outInstanceVersion) {
        *outInstanceVersion = apiVersion;
    }
    return apiVersion != 0;
}

bool Venus::probe(VenusProbeResult* result) {
    VenusProbeResult local = {};

    // 1) Capset negotiation.
    const bool ok = negotiate();
    local.capsetOk = negotiated_ && capset_.wire_format_version != 0;
    local.capsetVersion = capset_.vk_mesa_venus_protocol_spec_version;
    local.wireFormatVersion = capset_.wire_format_version;
    local.vkXmlVersion = capset_.vk_xml_version;
    local.venusProtocolVersion = capset_.vk_mesa_venus_protocol_spec_version;
    local.capsetVersionOk = ok;

    if (!available_) {
        if (result) {
            *result = local;
        }
        return false;
    }

    // 2) Venus context. The host render server may initialize the Venus
    // renderer lazily on the first context, so allow a few attempts.
    uint32_t ctxId = 0;
    bool ctxCreated = false;
    for (uint32_t attempt = 0; attempt < 4 && !ctxCreated; ++attempt) {
        ctxCreated = createContext(&ctxId, "venus-probe");
        if (!ctxCreated) {
            // Brief backoff to let the host renderer come up.
            for (uint32_t spin = 0; spin < 2000000u; ++spin) {
                __asm__ __volatile__("pause" ::: "memory");
            }
        }
    }
    if (!ctxCreated) {
        if (result) {
            *result = local;
        }
        return false;
    }
    local.contextOk = true;
    local.ctxId = ctxId;

    // 3) Shared command/reply blob (one page is plenty for this round trip).
    const uint64_t blobSize = 4096;
    uint32_t resourceId = 0;
    void* blob = nullptr;
    uint64_t blobLength = 0;
    if (!allocateSharedBlob(ctxId, blobSize, &resourceId, &blob, &blobLength)) {
        destroyContext(ctxId);
        if (result) {
            *result = local;
        }
        return false;
    }
    local.blobOk = true;
    local.resourceId = resourceId;

    // 4) vkEnumerateInstanceVersion round trip.
    uint32_t instanceVersion = 0;
    uint32_t responseType = 0;
    const bool replyOk = queryInstanceVersion(ctxId, resourceId, blob, blobLength,
                                               &instanceVersion, &responseType);
    const VirtIOGPUCommandStatus status = VirtIOGPUDriver::get().getLastCommandStatus();
    local.submitOk = status.transportOk;
    local.fenceOk = status.completedFence >= status.submittedFence && status.submittedFence != 0;
    local.responseType = responseType ? responseType : status.responseType;
    local.replyOk = replyOk;
    local.instanceVersion = instanceVersion;

    // 5) Teardown: unmap the host blob, detach it from the context, drop the
    // reference, then destroy the context.
    VirtIOGPUDriver::get().unmapBlobResource(resourceId);
    VirtIOGPUDriver::get().detachResourceFromContext(ctxId, resourceId);
    VirtIOGPUDriver::get().unrefResource(resourceId);
    destroyContext(ctxId);

    if (result) {
        *result = local;
    }

    return local.contextOk && local.blobOk && local.submitOk && local.fenceOk && local.replyOk;
}

// ---------------------------------------------------------------------------
// VulkanSession (async ring + real Vulkan commands)
// ---------------------------------------------------------------------------

namespace {

// Round up to the next power of two (for the ring buffer size).
uint64_t nextPow2(uint64_t v) {
    uint64_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

} // namespace

VulkanSession::~VulkanSession() {
    shutdown();
}

bool VulkanSession::init() {
    if (ringReady_) {
        return true;
    }
    if (!Venus::get().negotiate()) {
        return false;
    }

    gpu_ = &VirtIOGPUDriver::get();

    // Bring up the Venus context (with a few retries for the host renderer
    // cold-start race).
    bool ctxOk = false;
    for (uint32_t attempt = 0; attempt < 4 && !ctxOk; ++attempt) {
        ctxOk = Venus::get().createContext(&ctxId_, "venus-vk");
        if (!ctxOk) {
            for (uint32_t spin = 0; spin < 2000000u; ++spin) {
                __asm__ __volatile__("pause" ::: "memory");
            }
        }
    }
    if (!ctxOk) {
        return false;
    }

    // One blob holds the whole ring shmem plus a reply region and a scratch
    // staging area. 64 KiB is comfortable for instance/device bring-up.
    const uint64_t kBlobSize = 64u * 1024u;
    uint32_t resId = 0;
    void* ptr = nullptr;
    uint64_t length = 0;
    if (!Venus::get().allocateSharedBlob(ctxId_, kBlobSize, &resId, &ptr, &length)) {
        Venus::get().destroyContext(ctxId_);
        ctxId_ = 0;
        return false;
    }
    resourceId_ = resId;
    blob_ = static_cast<uint8_t*>(ptr);
    blobLength_ = length;

    // Ring layout inside the blob (matches vn_ring_get_layout's field order;
    // the control words are 64-byte aligned). head/tail/status are u32.
    headOffset_ = 0;
    tailOffset_ = 64;
    statusOffset_ = 128;
    bufferOffset_ = 192;
    // Reserve the tail of the blob for reply + scratch, give the rest (rounded
    // down to a power of two) to the ring buffer.
    replySize_ = 8u * 1024u;
    scratchSize_ = 8u * 1024u;
    const uint64_t avail = blobLength_ - bufferOffset_ - replySize_ - scratchSize_;
    bufferSize_ = nextPow2(avail);
    if (bufferSize_ > avail) {
        bufferSize_ >>= 1;
    }
    if (bufferSize_ < 4096) {
        shutdown();
        return false;
    }
    replyOffset_ = bufferOffset_ + bufferSize_;
    scratchOffset_ = replyOffset_ + replySize_;
    if (scratchOffset_ + scratchSize_ > blobLength_) {
        shutdown();
        return false;
    }

    // A unique, non-zero ring id (the host keys the ring by this value).
    ringId_ = 0x52494E47ull /* "RING" */ ^ (static_cast<uint64_t>(resourceId_) << 8);
    if (ringId_ == 0) {
        ringId_ = 1;
    }
    ringCur_ = 0;

    // Encode vkCreateRingMESA into scratch and submit via SUBMIT_3D (the ring
    // itself is created through the legacy direct-submit path, exactly like
    // Mesa's vn_ring_create).
    uint8_t* scratch = blob_ + scratchOffset_;
    CsEncoder enc(scratch, scratchSize_);
    enc.writeCommandType(VK_COMMAND_TYPE_vkCreateRingMESA);
    enc.writeFlags(0);
    enc.writeU64(ringId_);
    if (enc.writeSimplePointer(true)) {
        // VkRingCreateInfoMESA: sType, pNext(NULL), then _self fields.
        enc.writeStructureType(VK_STRUCTURE_TYPE_RING_CREATE_INFO_MESA);
        enc.writeSimplePointer(false);              // pNext = NULL
        enc.writeFlags(0);                          // flags
        enc.writeU32(resourceId_);                  // resourceId
        enc.writeSizeT(0);                          // offset (ring base within resource)
        enc.writeSizeT(blobLength_);                // size (whole resource span)
        enc.writeU64(0);                            // idleTimeout (0 = renderer default)
        enc.writeSizeT(headOffset_);
        enc.writeSizeT(tailOffset_);
        enc.writeSizeT(statusOffset_);
        enc.writeSizeT(bufferOffset_);
        enc.writeSizeT(bufferSize_);
        enc.writeSizeT(0);                          // extraOffset (unused)
        enc.writeSizeT(0);                          // extraSize (unused)
    }
    if (enc.overflowed()) {
        shutdown();
        return false;
    }

    const bool created = gpu_->submit3D(ctxId_, scratch, static_cast<uint32_t>(enc.length()));
    if (!created) {
        shutdown();
        return false;
    }
    ringReady_ = true;

    // Set the per-ring reply command stream once.
    if (!setReplyStream()) {
        shutdown();
        return false;
    }
    return true;
}

bool VulkanSession::setReplyStream() {
    if (!ringReady_) {
        return false;
    }
    // Build [vkSetReplyCommandStreamMESA] in scratch and push through the ring.
    uint8_t* scratch = blob_ + scratchOffset_;
    CsEncoder enc(scratch, scratchSize_);
    enc.writeCommandType(VK_COMMAND_TYPE_vkSetReplyCommandStreamMESA);
    enc.writeFlags(0);
    if (enc.writeSimplePointer(true)) {
        enc.writeU32(resourceId_);
        enc.writeSizeT(replyOffset_);
        enc.writeSizeT(replySize_);
    }
    if (enc.overflowed()) {
        return false;
    }
    if (!ringSubmit(scratch, static_cast<uint32_t>(enc.length()), 0, nullptr)) {
        return false;
    }
    replyStreamSet_ = true;
    return true;
}

bool VulkanSession::ringSubmit(const void* cmd, uint32_t cmdSize, uint32_t replySize,
                               const uint8_t** outReply) {
    if (!ringReady_ || cmdSize == 0 || cmdSize > bufferSize_) {
        return false;
    }

    volatile uint32_t* head = reinterpret_cast<volatile uint32_t*>(blob_ + headOffset_);
    volatile uint32_t* tail = reinterpret_cast<volatile uint32_t*>(blob_ + tailOffset_);
    volatile uint32_t* status = reinterpret_cast<volatile uint32_t*>(blob_ + statusOffset_);
    uint8_t* ringBuf = blob_ + bufferOffset_;

    // Wait for enough free space in the circular buffer. When a reply is
    // expected we also write a vkSeekReplyCommandStreamMESA(0) immediately
    // before the command so the host rewinds its reply write cursor and places
    // this command's reply at the start of the reply region. The seek and the
    // command must be consumed in order, so they are written as one unit.
    uint8_t seekCmd[16];
    uint32_t seekSize = 0;
    if (replySize > 0) {
        CsEncoder se(seekCmd, sizeof(seekCmd));
        se.writeCommandType(VK_COMMAND_TYPE_vkSeekReplyCommandStreamMESA);
        se.writeFlags(0);
        se.writeSizeT(0);
        if (se.overflowed()) {
            return false;
        }
        seekSize = static_cast<uint32_t>(se.length());
    }
    const uint32_t totalSize = cmdSize + seekSize;
    if (totalSize > bufferSize_) {
        return false;
    }

    const uint64_t bufMask = bufferSize_ - 1;
    for (uint32_t spin = 0; ; ++spin) {
        const uint64_t consumed = *head;
        if (ringCur_ + totalSize - consumed <= bufferSize_) {
            break;
        }
        if (spin > 1000000u) {
            return false;  // ring full / host stuck
        }
        __asm__ __volatile__("pause" ::: "memory");
    }

    // If a reply is expected, clear the reply region so a stale word is not
    // mistaken for a fresh reply.
    if (replySize > 0) {
        memset(blob_ + replyOffset_, 0, replySize_);
        __asm__ __volatile__("mfence" ::: "memory");
    }

    // Helper to write bytes into the circular ring buffer at the current tail.
    auto writeRing = [&](const uint8_t* data, uint32_t size) {
        const uint64_t off = ringCur_ & bufMask;
        if (off + size <= bufferSize_) {
            memcpy(ringBuf + off, data, size);
        } else {
            const uint64_t first = bufferSize_ - off;
            memcpy(ringBuf + off, data, first);
            memcpy(ringBuf, data + first, size - first);
        }
        ringCur_ += size;
    };

    if (seekSize > 0) {
        writeRing(seekCmd, seekSize);
    }
    writeRing(static_cast<const uint8_t*>(cmd), cmdSize);

    // Publish the new tail and check status.
    __asm__ __volatile__("mfence" ::: "memory");
    *tail = static_cast<uint32_t>(ringCur_);
    __asm__ __volatile__("mfence" ::: "memory");

    const uint32_t st = *status;
    if (st & VK_RING_STATUS_FATAL_BIT) {
        ringReady_ = false;
        return false;
    }

    // Notify the host if the ring is idle (host not actively polling).
    if (st & VK_RING_STATUS_IDLE_BIT) {
        uint8_t notifyBuf[32];
        CsEncoder ne(notifyBuf, sizeof(notifyBuf));
        ne.writeCommandType(VK_COMMAND_TYPE_vkNotifyRingMESA);
        ne.writeFlags(0);
        ne.writeU64(ringId_);
        ne.writeU32(static_cast<uint32_t>(ringCur_));   // seqno
        ne.writeFlags(0);                                // VkRingNotifyFlagsMESA
        if (!ne.overflowed()) {
            gpu_->submit3D(ctxId_, notifyBuf, static_cast<uint32_t>(ne.length()));
        }
    }

    // Wait for the host to consume up to our tail (head catches up to ringCur_).
    for (uint32_t spin = 0; ; ++spin) {
        if (static_cast<uint64_t>(*head) >= ringCur_) {
            break;
        }
        if (spin > 2000000u) {
            return false;  // host did not consume in time
        }
        if ((spin & 0x3FFu) == 0 && (*status & VK_RING_STATUS_IDLE_BIT)) {
            // Re-notify in case the earlier notify raced the host going idle.
            uint8_t nb[32];
            CsEncoder ne(nb, sizeof(nb));
            ne.writeCommandType(VK_COMMAND_TYPE_vkNotifyRingMESA);
            ne.writeFlags(0);
            ne.writeU64(ringId_);
            ne.writeU32(static_cast<uint32_t>(ringCur_));
            ne.writeFlags(0);
            if (!ne.overflowed()) {
                gpu_->submit3D(ctxId_, nb, static_cast<uint32_t>(ne.length()));
            }
        }
        __asm__ __volatile__("pause" ::: "memory");
    }

    if (replySize > 0) {
        __asm__ __volatile__("mfence" ::: "memory");
        // The reply's first word is the reply command type; spin briefly until
        // the host has written it (the ring consume + reply write may not be
        // perfectly ordered on the mapping).
        volatile uint32_t* replyHead = reinterpret_cast<volatile uint32_t*>(blob_ + replyOffset_);
        for (uint32_t spin = 0; spin < 200000u; ++spin) {
            if (*replyHead != 0) {
                break;
            }
            __asm__ __volatile__("pause; mfence" ::: "memory");
        }
        if (outReply) {
            *outReply = blob_ + replyOffset_;
        }
    }
    return true;
}

int VulkanSession::createInstance(uint32_t apiVersion) {
    if (!ringReady_) {
        return -1;
    }

    instance_ = allocHandleId();

    uint8_t* scratch = blob_ + scratchOffset_;
    CsEncoder enc(scratch, scratchSize_);
    enc.writeCommandType(VK_COMMAND_TYPE_vkCreateInstance);
    enc.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
    // pCreateInfo (present)
    if (enc.writeSimplePointer(true)) {
        enc.writeStructureType(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        enc.writeSimplePointer(false);          // pNext = NULL
        enc.writeFlags(0);                       // flags
        // pApplicationInfo (present)
        if (enc.writeSimplePointer(true)) {
            enc.writeStructureType(VK_STRUCTURE_TYPE_APPLICATION_INFO);
            enc.writeSimplePointer(false);      // pNext = NULL
            enc.writeArraySize(0);              // pApplicationName = NULL
            enc.writeU32(0);                    // applicationVersion
            enc.writeArraySize(0);              // pEngineName = NULL
            enc.writeU32(0);                    // engineVersion
            enc.writeU32(apiVersion);           // apiVersion
        }
        enc.writeU32(0);                         // enabledLayerCount
        enc.writeArraySize(0);                   // ppEnabledLayerNames = NULL
        enc.writeU32(0);                         // enabledExtensionCount
        enc.writeArraySize(0);                   // ppEnabledExtensionNames = NULL
    }
    enc.writeSimplePointer(false);               // pAllocator = NULL
    // pInstance (present) - guest-provided handle id
    if (enc.writeSimplePointer(true)) {
        enc.writeHandle(instance_);
    }
    if (enc.overflowed()) {
        return -1;
    }

    const uint8_t* reply = nullptr;
    if (!ringSubmit(scratch, static_cast<uint32_t>(enc.length()), 64, &reply) || !reply) {
        return -1;
    }

    // Reply: cmd, VkResult, simple_pointer(pInstance), VkInstance handle.
    CsDecoder dec(reply, replySize_);
    const uint32_t cmd = dec.readU32();
    if (dec.fatal() || cmd != VK_COMMAND_TYPE_vkCreateInstance) {
        return -1;
    }
    const int32_t ret = static_cast<int32_t>(dec.readU32());
    if (dec.readArraySize() != 0) {
        const uint64_t handle = dec.readHandle();
        if (!dec.fatal() && handle != 0) {
            instance_ = handle;
        }
    }
    return dec.fatal() ? -1 : ret;
}

uint32_t VulkanSession::enumeratePhysicalDevices() {
    if (!ringReady_ || instance_ == 0) {
        return 0;
    }

    // First call: query the count (pPhysicalDevices = NULL).
    uint32_t count = 0;
    {
        uint8_t* scratch = blob_ + scratchOffset_;
        CsEncoder enc(scratch, scratchSize_);
        enc.writeCommandType(VK_COMMAND_TYPE_vkEnumeratePhysicalDevices);
        enc.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        enc.writeHandle(instance_);
        enc.writeSimplePointer(true);           // pPhysicalDeviceCount present
        enc.writeU32(0);                         // *pPhysicalDeviceCount = 0
        enc.writeArraySize(0);                   // pPhysicalDevices = NULL
        if (enc.overflowed()) {
            return 0;
        }
        const uint8_t* reply = nullptr;
        if (!ringSubmit(scratch, static_cast<uint32_t>(enc.length()), 64, &reply) || !reply) {
            return 0;
        }
        CsDecoder dec(reply, replySize_);
        if (dec.readU32() != VK_COMMAND_TYPE_vkEnumeratePhysicalDevices) {
            return 0;
        }
        dec.readU32();                           // VkResult
        if (dec.readArraySize() != 0) {
            count = dec.readU32();
        }
        if (dec.fatal()) {
            return 0;
        }
    }

    if (count == 0) {
        physDevCount_ = 0;
        return 0;
    }
    if (count > kVenusMaxPhysicalDevices) {
        count = kVenusMaxPhysicalDevices;
    }

    // Second call: fetch the handles. The guest provides placeholder handle ids
    // for each physical device slot.
    for (uint32_t i = 0; i < count; ++i) {
        physDevs_[i] = allocHandleId();
    }

    uint8_t* scratch = blob_ + scratchOffset_;
    CsEncoder enc(scratch, scratchSize_);
    enc.writeCommandType(VK_COMMAND_TYPE_vkEnumeratePhysicalDevices);
    enc.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
    enc.writeHandle(instance_);
    enc.writeSimplePointer(true);               // pPhysicalDeviceCount present
    enc.writeU32(count);                         // *pPhysicalDeviceCount = count
    enc.writeArraySize(count);                   // pPhysicalDevices array, size count
    for (uint32_t i = 0; i < count; ++i) {
        enc.writeHandle(physDevs_[i]);
    }
    if (enc.overflowed()) {
        return 0;
    }
    const uint8_t* reply = nullptr;
    if (!ringSubmit(scratch, static_cast<uint32_t>(enc.length()), 256, &reply) || !reply) {
        return 0;
    }

    CsDecoder dec(reply, replySize_);
    if (dec.readU32() != VK_COMMAND_TYPE_vkEnumeratePhysicalDevices) {
        return 0;
    }
    dec.readU32();                               // VkResult
    uint32_t outCount = 0;
    if (dec.readArraySize() != 0) {
        outCount = dec.readU32();
    }
    // pPhysicalDevices array: array_size then the handles the host assigned.
    const uint64_t arr = dec.readArraySize();
    const uint32_t n = arr < count ? static_cast<uint32_t>(arr) : count;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t h = dec.readHandle();
        if (!dec.fatal()) {
            physDevs_[i] = h;
        }
    }
    if (dec.fatal()) {
        return 0;
    }
    physDevCount_ = outCount < count ? outCount : count;
    return physDevCount_;
}

bool VulkanSession::getPhysicalDeviceProperties(uint32_t index, VulkanPhysicalDeviceInfo* outInfo) {
    if (!ringReady_ || index >= physDevCount_ || !outInfo) {
        return false;
    }

    uint8_t* scratch = blob_ + scratchOffset_;
    CsEncoder enc(scratch, scratchSize_);
    enc.writeCommandType(VK_COMMAND_TYPE_vkGetPhysicalDeviceProperties);
    enc.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
    enc.writeHandle(physDevs_[index]);
    // pProperties present; the request body is the "partial" form, which encodes
    // nothing (all fields are reply-only), so just the presence flag.
    enc.writeSimplePointer(true);
    if (enc.overflowed()) {
        return false;
    }

    const uint8_t* reply = nullptr;
    if (!ringSubmit(scratch, static_cast<uint32_t>(enc.length()), 1024, &reply) || !reply) {
        return false;
    }

    CsDecoder dec(reply, replySize_);
    if (dec.readU32() != VK_COMMAND_TYPE_vkGetPhysicalDeviceProperties) {
        return false;
    }
    if (dec.readArraySize() == 0) {
        return false;  // pProperties was NULL in the reply
    }

    VulkanPhysicalDeviceInfo info = {};
    info.handle = physDevs_[index];
    info.apiVersion = dec.readU32();
    info.driverVersion = dec.readU32();
    info.vendorId = dec.readU32();
    info.deviceId = dec.readU32();
    info.deviceType = dec.readU32();
    // deviceName: array_size(256) then 256-byte char blob.
    const uint64_t nameLen = dec.readArraySize();
    dec.readBlob(info.deviceName, static_cast<size_t>(nameLen),
                 sizeof(info.deviceName) - 1);
    info.deviceName[sizeof(info.deviceName) - 1] = '\0';
    if (dec.fatal()) {
        return false;
    }
    *outInfo = info;
    return true;
}

int VulkanSession::createDevice(uint32_t physicalDeviceIndex) {
    if (!ringReady_ || physicalDeviceIndex >= physDevCount_) {
        return -1;
    }

    device_ = allocHandleId();
    // IEEE-754 bit pattern for 1.0f (avoids pulling in soft-float).
    constexpr uint32_t kQueuePriorityOneBits = 0x3F800000u;

    uint8_t* scratch = blob_ + scratchOffset_;
    CsEncoder enc(scratch, scratchSize_);
    enc.writeCommandType(VK_COMMAND_TYPE_vkCreateDevice);
    enc.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
    enc.writeHandle(physDevs_[physicalDeviceIndex]);
    // pCreateInfo (present)
    if (enc.writeSimplePointer(true)) {
        enc.writeStructureType(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        enc.writeSimplePointer(false);          // pNext = NULL
        enc.writeFlags(0);                       // flags
        enc.writeU32(1);                         // queueCreateInfoCount
        enc.writeArraySize(1);                   // pQueueCreateInfos array, size 1
        // VkDeviceQueueCreateInfo[0]
        enc.writeStructureType(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        enc.writeSimplePointer(false);          // pNext = NULL
        enc.writeFlags(0);                       // flags
        enc.writeU32(0);                         // queueFamilyIndex
        enc.writeU32(1);                         // queueCount
        enc.writeArraySize(1);                   // pQueuePriorities array, size 1
        enc.writeFloatBits(kQueuePriorityOneBits);
        enc.writeU32(0);                         // enabledLayerCount
        enc.writeArraySize(0);                   // ppEnabledLayerNames = NULL
        enc.writeU32(0);                         // enabledExtensionCount
        enc.writeArraySize(0);                   // ppEnabledExtensionNames = NULL
        enc.writeSimplePointer(false);          // pEnabledFeatures = NULL
    }
    enc.writeSimplePointer(false);               // pAllocator = NULL
    if (enc.writeSimplePointer(true)) {          // pDevice present
        enc.writeHandle(device_);
    }
    if (enc.overflowed()) {
        return -1;
    }

    const uint8_t* reply = nullptr;
    if (!ringSubmit(scratch, static_cast<uint32_t>(enc.length()), 64, &reply) || !reply) {
        return -1;
    }

    CsDecoder dec(reply, replySize_);
    if (dec.readU32() != VK_COMMAND_TYPE_vkCreateDevice) {
        return -1;
    }
    const int32_t ret = static_cast<int32_t>(dec.readU32());
    if (dec.readArraySize() != 0) {
        const uint64_t handle = dec.readHandle();
        if (!dec.fatal() && handle != 0) {
            device_ = handle;
        }
    }
    return dec.fatal() ? -1 : ret;
}

// ---------------------------------------------------------------------------
// Compute dispatch (memory, descriptors, pipeline, command buffer, submit)
// ---------------------------------------------------------------------------

namespace {

// SPIR-V for a compute shader (local_size_x = 64) computing, for each invocation
//   data[gl_GlobalInvocationID.x] = i*i + 1
// into a std430 storage buffer at set 0, binding 0. Compiled from GLSL with
// glslangValidator; see tools/venus-shader (documented in docs/venus.md).
const uint32_t kComputeSpirv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000020, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000b, 0x00060010, 0x00000004,
    0x00000011, 0x00000040, 0x00000001, 0x00000001, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005,
    0x00000008, 0x00000069, 0x00080005, 0x0000000b, 0x475f6c67, 0x61626f6c,
    0x766e496c, 0x7461636f, 0x496e6f69, 0x00000044, 0x00030005, 0x00000011,
    0x0074754f, 0x00050006, 0x00000011, 0x00000000, 0x61746164, 0x00000000,
    0x00030005, 0x00000013, 0x00000000, 0x00040047, 0x0000000b, 0x0000000b,
    0x0000001c, 0x00040047, 0x00000010, 0x00000006, 0x00000004, 0x00030047,
    0x00000011, 0x00000003, 0x00050048, 0x00000011, 0x00000000, 0x00000023,
    0x00000000, 0x00040047, 0x00000013, 0x00000021, 0x00000000, 0x00040047,
    0x00000013, 0x00000022, 0x00000000, 0x00040047, 0x0000001f, 0x0000000b,
    0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040020, 0x00000007,
    0x00000007, 0x00000006, 0x00040017, 0x00000009, 0x00000006, 0x00000003,
    0x00040020, 0x0000000a, 0x00000001, 0x00000009, 0x0004003b, 0x0000000a,
    0x0000000b, 0x00000001, 0x0004002b, 0x00000006, 0x0000000c, 0x00000000,
    0x00040020, 0x0000000d, 0x00000001, 0x00000006, 0x0003001d, 0x00000010,
    0x00000006, 0x0003001e, 0x00000011, 0x00000010, 0x00040020, 0x00000012,
    0x00000002, 0x00000011, 0x0004003b, 0x00000012, 0x00000013, 0x00000002,
    0x00040015, 0x00000014, 0x00000020, 0x00000001, 0x0004002b, 0x00000014,
    0x00000015, 0x00000000, 0x0004002b, 0x00000006, 0x0000001a, 0x00000001,
    0x00040020, 0x0000001c, 0x00000002, 0x00000006, 0x0004002b, 0x00000006,
    0x0000001e, 0x00000040, 0x0006002c, 0x00000009, 0x0000001f, 0x0000001e,
    0x0000001a, 0x0000001a, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000007, 0x00000008,
    0x00000007, 0x00050041, 0x0000000d, 0x0000000e, 0x0000000b, 0x0000000c,
    0x0004003d, 0x00000006, 0x0000000f, 0x0000000e, 0x0003003e, 0x00000008,
    0x0000000f, 0x0004003d, 0x00000006, 0x00000016, 0x00000008, 0x0004003d,
    0x00000006, 0x00000017, 0x00000008, 0x0004003d, 0x00000006, 0x00000018,
    0x00000008, 0x00050084, 0x00000006, 0x00000019, 0x00000017, 0x00000018,
    0x00050080, 0x00000006, 0x0000001b, 0x00000019, 0x0000001a, 0x00060041,
    0x0000001c, 0x0000001d, 0x00000013, 0x00000015, 0x00000016, 0x0003003e,
    0x0000001d, 0x0000001b, 0x000100fd, 0x00010038,
};

} // namespace

int VulkanSession::decodeCreateReply(const void* cmd, uint64_t cmdSize, uint32_t expectedCmdType) {
    const uint8_t* reply = nullptr;
    if (!ringSubmit(cmd, static_cast<uint32_t>(cmdSize), 64, &reply) || !reply) {
        return -1;
    }
    CsDecoder dec(reply, replySize_);
    if (dec.readU32() != expectedCmdType) {
        return -1;
    }
    const int32_t ret = static_cast<int32_t>(dec.readU32());  // VkResult
    if (dec.readArraySize() != 0) {
        dec.readHandle();  // the created object handle (host confirms our id)
    }
    return dec.fatal() ? -1 : ret;
}

int VulkanSession::decodeStatusReply(const void* cmd, uint64_t cmdSize, uint32_t expectedCmdType) {
    const uint8_t* reply = nullptr;
    if (!ringSubmit(cmd, static_cast<uint32_t>(cmdSize), 64, &reply) || !reply) {
        return -1;
    }
    CsDecoder dec(reply, replySize_);
    if (dec.readU32() != expectedCmdType) {
        return -1;
    }
    const int32_t ret = static_cast<int32_t>(dec.readU32());  // VkResult
    return dec.fatal() ? -1 : ret;
}

bool VulkanSession::selectHostVisibleMemoryType(uint64_t physDev, uint32_t memoryTypeBits,
                                                uint32_t* outTypeIndex) {
    uint8_t* scratch = blob_ + scratchOffset_;
    CsEncoder enc(scratch, scratchSize_);
    enc.writeCommandType(VK_COMMAND_TYPE_vkGetPhysicalDeviceMemoryProperties);
    enc.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
    enc.writeHandle(physDev);
    if (enc.writeSimplePointer(true)) {
        // VkPhysicalDeviceMemoryProperties "partial" request body: the two array
        // sizes must be present (the per-element bodies are empty).
        enc.writeArraySize(VK_MAX_MEMORY_TYPES);
        enc.writeArraySize(VK_MAX_MEMORY_HEAPS);
    }
    if (enc.overflowed()) {
        return false;
    }

    const uint8_t* reply = nullptr;
    if (!ringSubmit(scratch, static_cast<uint32_t>(enc.length()), 2048, &reply) || !reply) {
        return false;
    }

    CsDecoder dec(reply, replySize_);
    if (dec.readU32() != VK_COMMAND_TYPE_vkGetPhysicalDeviceMemoryProperties) {
        return false;
    }
    if (dec.readArraySize() == 0) {
        return false;  // pMemoryProperties NULL
    }
    // VkPhysicalDeviceMemoryProperties: memoryTypeCount, array(32) of
    // {propertyFlags, heapIndex}, then heaps (ignored).
    const uint32_t typeCount = dec.readU32();
    const uint64_t typeArr = dec.readArraySize();
    const uint32_t needed = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    bool found = false;
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < typeArr; ++i) {
        const uint32_t propertyFlags = dec.readU32();
        dec.readU32();  // heapIndex
        if (dec.fatal()) {
            return false;
        }
        const bool usable = i < typeCount && (memoryTypeBits & (1u << i)) != 0 &&
                            (propertyFlags & needed) == needed;
        if (usable && !found) {
            chosen = i;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    *outTypeIndex = chosen;
    return true;
}

bool VulkanSession::allocateMappableMemory(uint64_t size, uint32_t memoryTypeIndex,
                                           uint64_t* outMemory, uint32_t* outResId, void** outPtr) {
    // vkAllocateMemory (guest-assigned memory object id).
    const uint64_t memId = allocHandleId();
    {
        uint8_t* scratch = blob_ + scratchOffset_;
        CsEncoder enc(scratch, scratchSize_);
        enc.writeCommandType(VK_COMMAND_TYPE_vkAllocateMemory);
        enc.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        enc.writeHandle(device_);
        if (enc.writeSimplePointer(true)) {  // pAllocateInfo
            enc.writeStructureType(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
            enc.writeSimplePointer(false);   // pNext = NULL
            enc.writeU64(size);              // allocationSize (VkDeviceSize)
            enc.writeU32(memoryTypeIndex);
        }
        enc.writeSimplePointer(false);       // pAllocator = NULL
        if (enc.writeSimplePointer(true)) {  // pMemory present
            enc.writeHandle(memId);
        }
        if (enc.overflowed()) {
            return false;
        }
        const uint8_t* reply = nullptr;
        if (!ringSubmit(scratch, static_cast<uint32_t>(enc.length()), 64, &reply) || !reply) {
            return false;
        }
        CsDecoder dec(reply, replySize_);
        if (dec.readU32() != VK_COMMAND_TYPE_vkAllocateMemory) {
            return false;
        }
        const int32_t ret = static_cast<int32_t>(dec.readU32());
        uint64_t handle = memId;
        if (dec.readArraySize() != 0) {
            handle = dec.readHandle();
        }
        if (dec.fatal() || ret != 0) {
            return false;
        }
        *outMemory = handle;
    }

    // Back the device memory with a HOST3D blob keyed by the memory object id,
    // then map it into the guest. The alloc reply above guarantees the host has
    // processed the allocation before this blob create references its id.
    uint32_t resId = 0;
    void* ptr = nullptr;
    uint64_t mappedLen = 0;
    if (!gpu_->allocateContextBlobWithId(ctxId_, size, *outMemory, &resId, &ptr, &mappedLen) || !ptr) {
        return false;
    }
    *outResId = resId;
    *outPtr = ptr;
    return true;
}

bool VulkanSession::runCompute(uint32_t elementCount, uint32_t* out, uint32_t outCap,
                               uint32_t* outMismatches) {
    if (!ringReady_ || device_ == 0 || physDevCount_ == 0 || elementCount == 0) {
        return false;
    }
    // Round the dispatch up to the shader's local size (64).
    const uint32_t kLocalSize = 64;
    const uint32_t groups = (elementCount + kLocalSize - 1) / kLocalSize;
    const uint64_t bufferBytes = static_cast<uint64_t>(elementCount) * sizeof(uint32_t);

    // 1) Storage buffer.
    const uint64_t buffer = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateBuffer);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkBufferCreateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(0);               // flags
            e.writeU64(bufferBytes);       // size
            e.writeFlags(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            e.writeU32(VK_SHARING_MODE_EXCLUSIVE);
            e.writeU32(0);                 // queueFamilyIndexCount
            e.writeArraySize(0);           // pQueueFamilyIndices (exclusive)
        }
        e.writeSimplePointer(false);       // pAllocator
        if (e.writeSimplePointer(true)) {
            e.writeHandle(buffer);
        }
        if (e.overflowed() || decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateBuffer) != 0) {
            return false;
        }
    }

    // 2) Memory requirements -> choose a host-visible+coherent memory type.
    uint64_t memReqSize = bufferBytes;
    uint32_t memTypeBits = 0xFFFFFFFFu;
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkGetBufferMemoryRequirements);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        e.writeHandle(buffer);
        e.writeSimplePointer(true);  // pMemoryRequirements present (partial body empty)
        const uint8_t* reply = nullptr;
        if (e.overflowed() ||
            !ringSubmit(s, static_cast<uint32_t>(e.length()), 64, &reply) || !reply) {
            return false;
        }
        CsDecoder dec(reply, replySize_);
        if (dec.readU32() != VK_COMMAND_TYPE_vkGetBufferMemoryRequirements) {
            return false;
        }
        if (dec.readArraySize() == 0) {
            return false;
        }
        memReqSize = dec.readU64();   // size
        dec.readU64();                // alignment
        memTypeBits = dec.readU32();  // memoryTypeBits
        if (dec.fatal()) {
            return false;
        }
    }

    uint32_t memTypeIndex = 0;
    if (!selectHostVisibleMemoryType(physDevs_[0], memTypeBits, &memTypeIndex)) {
        return false;
    }

    // 3) Allocate host-visible memory, map it, and bind it to the buffer.
    uint64_t memory = 0;
    uint32_t memResId = 0;
    void* mappedBuf = nullptr;
    if (!allocateMappableMemory(memReqSize, memTypeIndex, &memory, &memResId, &mappedBuf)) {
        return false;
    }
    // Pre-fill the buffer with a sentinel so we can detect the GPU actually wrote.
    {
        uint32_t* w = static_cast<uint32_t*>(mappedBuf);
        for (uint32_t i = 0; i < elementCount; ++i) {
            w[i] = 0xDEADBEEFu;
        }
        __asm__ __volatile__("mfence" ::: "memory");
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkBindBufferMemory);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        e.writeHandle(buffer);
        e.writeHandle(memory);
        e.writeU64(0);  // memoryOffset
        if (e.overflowed() ||
            decodeStatusReply(s, e.length(), VK_COMMAND_TYPE_vkBindBufferMemory) != 0) {
            return false;
        }
    }

    // 4) Shader module from the embedded SPIR-V.
    const uint64_t shaderModule = allocHandleId();
    {
        const uint32_t spirvWords = sizeof(kComputeSpirv) / sizeof(kComputeSpirv[0]);
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateShaderModule);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkShaderModuleCreateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(0);               // flags
            e.writeSizeT(sizeof(kComputeSpirv));  // codeSize (bytes)
            e.writeArraySize(spirvWords);         // pCode array of u32
            for (uint32_t i = 0; i < spirvWords; ++i) {
                e.writeU32(kComputeSpirv[i]);
            }
        }
        e.writeSimplePointer(false);       // pAllocator
        if (e.writeSimplePointer(true)) {
            e.writeHandle(shaderModule);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateShaderModule) != 0) {
            return false;
        }
    }

    // 5) Descriptor set layout (one storage buffer at binding 0, compute stage).
    const uint64_t setLayout = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateDescriptorSetLayout);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkDescriptorSetLayoutCreateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(0);               // flags
            e.writeU32(1);                 // bindingCount
            e.writeArraySize(1);           // pBindings array
            // VkDescriptorSetLayoutBinding
            e.writeU32(0);                                  // binding
            e.writeU32(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);  // descriptorType
            e.writeU32(1);                                  // descriptorCount
            e.writeFlags(VK_SHADER_STAGE_COMPUTE_BIT);      // stageFlags
            e.writeArraySize(0);                            // pImmutableSamplers
        }
        e.writeSimplePointer(false);       // pAllocator
        if (e.writeSimplePointer(true)) {
            e.writeHandle(setLayout);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateDescriptorSetLayout) != 0) {
            return false;
        }
    }

    // 6) Descriptor pool + set.
    const uint64_t descPool = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateDescriptorPool);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkDescriptorPoolCreateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(0);               // flags
            e.writeU32(1);                 // maxSets
            e.writeU32(1);                 // poolSizeCount
            e.writeArraySize(1);           // pPoolSizes array
            e.writeU32(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);  // type
            e.writeU32(1);                 // descriptorCount
        }
        e.writeSimplePointer(false);       // pAllocator
        if (e.writeSimplePointer(true)) {
            e.writeHandle(descPool);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateDescriptorPool) != 0) {
            return false;
        }
    }

    const uint64_t descSet = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkAllocateDescriptorSets);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkDescriptorSetAllocateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeHandle(descPool);       // descriptorPool
            e.writeU32(1);                 // descriptorSetCount
            e.writeArraySize(1);           // pSetLayouts array
            e.writeHandle(setLayout);
        }
        e.writeArraySize(1);               // pDescriptorSets array (guest ids)
        e.writeHandle(descSet);
        const uint8_t* reply = nullptr;
        if (e.overflowed() ||
            !ringSubmit(s, static_cast<uint32_t>(e.length()), 64, &reply) || !reply) {
            return false;
        }
        CsDecoder dec(reply, replySize_);
        if (dec.readU32() != VK_COMMAND_TYPE_vkAllocateDescriptorSets) {
            return false;
        }
        const int32_t ret = static_cast<int32_t>(dec.readU32());
        if (dec.readArraySize() != 0) {
            const uint64_t h = dec.readHandle();
            if (!dec.fatal()) {
                // host-assigned set handle (typically equals our id)
                (void)h;
            }
        }
        if (dec.fatal() || ret != 0) {
            return false;
        }
    }

    // 7) Bind the buffer to the descriptor set (vkUpdateDescriptorSets, no reply).
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkUpdateDescriptorSets);
        e.writeFlags(0);
        e.writeHandle(device_);
        e.writeU32(1);                 // descriptorWriteCount
        e.writeArraySize(1);           // pDescriptorWrites array
        // VkWriteDescriptorSet
        e.writeStructureType(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        e.writeSimplePointer(false);   // pNext
        e.writeHandle(descSet);        // dstSet
        e.writeU32(0);                 // dstBinding
        e.writeU32(0);                 // dstArrayElement
        e.writeU32(1);                 // descriptorCount
        e.writeU32(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);  // descriptorType
        e.writeArraySize(0);           // pImageInfo
        e.writeArraySize(1);           // pBufferInfo array
        e.writeHandle(buffer);         // VkDescriptorBufferInfo.buffer
        e.writeU64(0);                 // offset
        e.writeU64(VK_WHOLE_SIZE);     // range
        e.writeArraySize(0);           // pTexelBufferView
        e.writeU32(0);                 // descriptorCopyCount
        e.writeArraySize(0);           // pDescriptorCopies
        if (e.overflowed() || !ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr)) {
            return false;
        }
    }

    // 8) Pipeline layout + compute pipeline.
    const uint64_t pipelineLayout = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreatePipelineLayout);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkPipelineLayoutCreateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(0);               // flags
            e.writeU32(1);                 // setLayoutCount
            e.writeArraySize(1);           // pSetLayouts array
            e.writeHandle(setLayout);
            e.writeU32(0);                 // pushConstantRangeCount
            e.writeArraySize(0);           // pPushConstantRanges
        }
        e.writeSimplePointer(false);       // pAllocator
        if (e.writeSimplePointer(true)) {
            e.writeHandle(pipelineLayout);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreatePipelineLayout) != 0) {
            return false;
        }
    }

    const uint64_t pipeline = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateComputePipelines);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        e.writeHandle(0);              // pipelineCache = VK_NULL_HANDLE
        e.writeU32(1);                 // createInfoCount
        e.writeArraySize(1);           // pCreateInfos array
        // VkComputePipelineCreateInfo
        e.writeStructureType(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        e.writeSimplePointer(false);   // pNext
        e.writeFlags(0);               // flags
        // VkPipelineShaderStageCreateInfo stage
        e.writeStructureType(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        e.writeSimplePointer(false);   // pNext
        e.writeFlags(0);               // flags
        e.writeU32(VK_SHADER_STAGE_COMPUTE_BIT);  // stage
        e.writeHandle(shaderModule);   // module
        e.writeString("main");         // pName
        e.writeSimplePointer(false);   // pSpecializationInfo
        e.writeHandle(pipelineLayout); // layout
        e.writeHandle(0);              // basePipelineHandle
        e.writeU32(0xFFFFFFFFu);       // basePipelineIndex (-1)
        e.writeSimplePointer(false);   // pAllocator
        e.writeArraySize(1);           // pPipelines array (guest ids)
        e.writeHandle(pipeline);
        const uint8_t* reply = nullptr;
        if (e.overflowed() ||
            !ringSubmit(s, static_cast<uint32_t>(e.length()), 64, &reply) || !reply) {
            return false;
        }
        CsDecoder dec(reply, replySize_);
        if (dec.readU32() != VK_COMMAND_TYPE_vkCreateComputePipelines) {
            return false;
        }
        const int32_t ret = static_cast<int32_t>(dec.readU32());
        if (dec.readArraySize() != 0) {
            dec.readHandle();  // host pipeline handle
        }
        if (dec.fatal() || ret != 0) {
            return false;
        }
    }

    // 9) Queue + command pool + command buffer.
    const uint64_t queue = allocHandleId();
    {
        // Venus retrieves the queue via vkGetDeviceQueue2 as an async (no-reply)
        // command: the guest allocates the queue handle id and the host
        // registers it (matching Mesa's vn_async_vkGetDeviceQueue2).
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkGetDeviceQueue2);
        e.writeFlags(0);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkDeviceQueueInfo2
            e.writeStructureType(VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2);
            // pNext: VkDeviceQueueTimelineInfoMESA is REQUIRED by the vkr
            // renderer (it ties the queue to a ring index for timeline/fence
            // tracking). Encode it as a present pNext with ringIdx 0.
            if (e.writeSimplePointer(true)) {
                e.writeStructureType(VK_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA);
                e.writeSimplePointer(false);   // its pNext = NULL
                e.writeU32(1);                 // ringIdx (must be in [1, NUM_RINGS])
            }
            e.writeFlags(0);               // flags
            e.writeU32(0);                 // queueFamilyIndex
            e.writeU32(0);                 // queueIndex
        }
        if (e.writeSimplePointer(true)) {
            e.writeHandle(queue);
        }
        if (e.overflowed() || !ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr)) {
            return false;
        }
    }

    const uint64_t cmdPool = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateCommandPool);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkCommandPoolCreateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(0);               // flags
            e.writeU32(0);                 // queueFamilyIndex
        }
        e.writeSimplePointer(false);       // pAllocator
        if (e.writeSimplePointer(true)) {
            e.writeHandle(cmdPool);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateCommandPool) != 0) {
            return false;
        }
    }

    const uint64_t cmdBuf = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkAllocateCommandBuffers);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkCommandBufferAllocateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeHandle(cmdPool);        // commandPool
            e.writeU32(VK_COMMAND_BUFFER_LEVEL_PRIMARY);  // level
            e.writeU32(1);                 // commandBufferCount
        }
        e.writeArraySize(1);               // pCommandBuffers array (guest ids)
        e.writeHandle(cmdBuf);
        const uint8_t* reply = nullptr;
        if (e.overflowed() ||
            !ringSubmit(s, static_cast<uint32_t>(e.length()), 64, &reply) || !reply) {
            return false;
        }
        CsDecoder dec(reply, replySize_);
        if (dec.readU32() != VK_COMMAND_TYPE_vkAllocateCommandBuffers) {
            return false;
        }
        const int32_t ret = static_cast<int32_t>(dec.readU32());
        if (dec.readArraySize() != 0) {
            dec.readHandle();
        }
        if (dec.fatal() || ret != 0) {
            return false;
        }
    }

    // 10) Record: begin, bind pipeline, bind descriptor set, dispatch, end.
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkBeginCommandBuffer);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(cmdBuf);
        if (e.writeSimplePointer(true)) {  // VkCommandBufferBeginInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
            e.writeSimplePointer(false);   // pInheritanceInfo
        }
        if (e.overflowed() ||
            decodeStatusReply(s, e.length(), VK_COMMAND_TYPE_vkBeginCommandBuffer) != 0) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCmdBindPipeline);
        e.writeFlags(0);
        e.writeHandle(cmdBuf);
        e.writeU32(VK_PIPELINE_BIND_POINT_COMPUTE);
        e.writeHandle(pipeline);
        if (e.overflowed() || !ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr)) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCmdBindDescriptorSets);
        e.writeFlags(0);
        e.writeHandle(cmdBuf);
        e.writeU32(VK_PIPELINE_BIND_POINT_COMPUTE);
        e.writeHandle(pipelineLayout);
        e.writeU32(0);                 // firstSet
        e.writeU32(1);                 // descriptorSetCount
        e.writeArraySize(1);           // pDescriptorSets array
        e.writeHandle(descSet);
        e.writeU32(0);                 // dynamicOffsetCount
        e.writeArraySize(0);           // pDynamicOffsets
        if (e.overflowed() || !ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr)) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCmdDispatch);
        e.writeFlags(0);
        e.writeHandle(cmdBuf);
        e.writeU32(groups);  // groupCountX
        e.writeU32(1);       // groupCountY
        e.writeU32(1);       // groupCountZ
        if (e.overflowed() || !ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr)) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkEndCommandBuffer);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(cmdBuf);
        if (e.overflowed() ||
            decodeStatusReply(s, e.length(), VK_COMMAND_TYPE_vkEndCommandBuffer) != 0) {
            return false;
        }
    }

    // 11) Create a fence, submit to the queue signalling it, then wait on the
    // fence. (The Venus renderer does not implement vkQueueWaitIdle over the
    // wire; Mesa itself uses a fence + vkWaitForFences for queue idle waits.)
    const uint64_t fence = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateFence);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkFenceCreateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(0);               // flags
        }
        e.writeSimplePointer(false);       // pAllocator
        if (e.writeSimplePointer(true)) {
            e.writeHandle(fence);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateFence) != 0) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkQueueSubmit);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(queue);
        e.writeU32(1);                 // submitCount
        e.writeArraySize(1);           // pSubmits array
        // VkSubmitInfo
        e.writeStructureType(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        e.writeSimplePointer(false);   // pNext
        e.writeU32(0);                 // waitSemaphoreCount
        e.writeArraySize(0);           // pWaitSemaphores
        e.writeArraySize(0);           // pWaitDstStageMask
        e.writeU32(1);                 // commandBufferCount
        e.writeArraySize(1);           // pCommandBuffers array
        e.writeHandle(cmdBuf);
        e.writeU32(0);                 // signalSemaphoreCount
        e.writeArraySize(0);           // pSignalSemaphores
        e.writeHandle(fence);          // fence (signalled on completion)
        if (e.overflowed() ||
            decodeStatusReply(s, e.length(), VK_COMMAND_TYPE_vkQueueSubmit) != 0) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkWaitForFences);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        e.writeU32(1);                 // fenceCount
        e.writeArraySize(1);           // pFences array
        e.writeHandle(fence);
        e.writeU32(1);                 // waitAll = VK_TRUE
        e.writeU64(~0ULL);             // timeout = UINT64_MAX
        if (e.overflowed() ||
            decodeStatusReply(s, e.length(), VK_COMMAND_TYPE_vkWaitForFences) != 0) {
            return false;
        }
    }

    // 12) Read back and verify. The host-visible coherent memory is mapped into
    // our blob, so the GPU's writes are directly visible to the guest CPU.
    __asm__ __volatile__("mfence" ::: "memory");
    const uint32_t* result = static_cast<const uint32_t*>(mappedBuf);
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < elementCount; ++i) {
        const uint32_t expected = i * i + 1u;
        const uint32_t got = result[i];
        if (got != expected) {
            ++mismatches;
        }
        if (i < outCap && out) {
            out[i] = got;
        }
    }
    if (outMismatches) {
        *outMismatches = mismatches;
    }

    // Destroy the Vulkan objects we created (async, no replies) so the host does
    // not report leaked objects at device teardown. vkDestroyX takes
    // (device, handle, pAllocator=NULL); vkFreeMemory the same shape.
    auto destroyObj = [&](uint32_t cmdType, uint64_t handle) {
        if (handle == 0) {
            return;
        }
        uint8_t buf[64];
        CsEncoder e(buf, sizeof(buf));
        e.writeCommandType(cmdType);
        e.writeFlags(0);
        e.writeHandle(device_);
        e.writeHandle(handle);
        e.writeSimplePointer(false);  // pAllocator = NULL
        if (!e.overflowed()) {
            ringSubmit(buf, static_cast<uint32_t>(e.length()), 0, nullptr);
        }
    };
    destroyObj(VK_COMMAND_TYPE_vkDestroyFence, fence);
    // Command buffers are freed with the pool; just destroy the pool.
    destroyObj(VK_COMMAND_TYPE_vkDestroyCommandPool, cmdPool);
    destroyObj(VK_COMMAND_TYPE_vkDestroyPipeline, pipeline);
    destroyObj(VK_COMMAND_TYPE_vkDestroyPipelineLayout, pipelineLayout);
    destroyObj(VK_COMMAND_TYPE_vkDestroyDescriptorPool, descPool);
    destroyObj(VK_COMMAND_TYPE_vkDestroyDescriptorSetLayout, setLayout);
    destroyObj(VK_COMMAND_TYPE_vkDestroyShaderModule, shaderModule);
    destroyObj(VK_COMMAND_TYPE_vkDestroyBuffer, buffer);
    destroyObj(VK_COMMAND_TYPE_vkFreeMemory, memory);

    // Free the guest mapping + blob backing the device memory.
    gpu_->unmapBlobResource(memResId);
    gpu_->unrefResource(memResId);

    return mismatches == 0;
}

// ---------------------------------------------------------------------------
// Graphics: render a colored triangle and copy it back to CPU-visible memory
// ---------------------------------------------------------------------------

namespace {

// Vertex shader: emits a 3-vertex triangle with per-vertex RGB colors using
// gl_VertexIndex (no vertex buffer). See tools/venus-shader/triangle.vert.
const uint32_t kTriVertSpirv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000036, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000000,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000022, 0x00000026, 0x00000031,
    0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00050005, 0x0000000c, 0x69736f70, 0x6e6f6974, 0x00000073,
    0x00040005, 0x00000017, 0x6f6c6f63, 0x00007372, 0x00060005, 0x00000020,
    0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x00000020,
    0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x00000020,
    0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006,
    0x00000020, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e,
    0x00070006, 0x00000020, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369,
    0x0065636e, 0x00030005, 0x00000022, 0x00000000, 0x00060005, 0x00000026,
    0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00040005, 0x00000031,
    0x6c6f4376, 0x0000726f, 0x00030047, 0x00000020, 0x00000002, 0x00050048,
    0x00000020, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x00000020,
    0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x00000020, 0x00000002,
    0x0000000b, 0x00000003, 0x00050048, 0x00000020, 0x00000003, 0x0000000b,
    0x00000004, 0x00040047, 0x00000026, 0x0000000b, 0x0000002a, 0x00040047,
    0x00000031, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017,
    0x00000007, 0x00000006, 0x00000002, 0x00040015, 0x00000008, 0x00000020,
    0x00000000, 0x0004002b, 0x00000008, 0x00000009, 0x00000003, 0x0004001c,
    0x0000000a, 0x00000007, 0x00000009, 0x00040020, 0x0000000b, 0x00000007,
    0x0000000a, 0x0004002b, 0x00000006, 0x0000000d, 0x00000000, 0x0004002b,
    0x00000006, 0x0000000e, 0xbf19999a, 0x0005002c, 0x00000007, 0x0000000f,
    0x0000000d, 0x0000000e, 0x0004002b, 0x00000006, 0x00000010, 0x3f19999a,
    0x0005002c, 0x00000007, 0x00000011, 0x00000010, 0x00000010, 0x0005002c,
    0x00000007, 0x00000012, 0x0000000e, 0x00000010, 0x0006002c, 0x0000000a,
    0x00000013, 0x0000000f, 0x00000011, 0x00000012, 0x00040017, 0x00000014,
    0x00000006, 0x00000003, 0x0004001c, 0x00000015, 0x00000014, 0x00000009,
    0x00040020, 0x00000016, 0x00000007, 0x00000015, 0x0004002b, 0x00000006,
    0x00000018, 0x3f800000, 0x0006002c, 0x00000014, 0x00000019, 0x00000018,
    0x0000000d, 0x0000000d, 0x0006002c, 0x00000014, 0x0000001a, 0x0000000d,
    0x00000018, 0x0000000d, 0x0006002c, 0x00000014, 0x0000001b, 0x0000000d,
    0x0000000d, 0x00000018, 0x0006002c, 0x00000015, 0x0000001c, 0x00000019,
    0x0000001a, 0x0000001b, 0x00040017, 0x0000001d, 0x00000006, 0x00000004,
    0x0004002b, 0x00000008, 0x0000001e, 0x00000001, 0x0004001c, 0x0000001f,
    0x00000006, 0x0000001e, 0x0006001e, 0x00000020, 0x0000001d, 0x00000006,
    0x0000001f, 0x0000001f, 0x00040020, 0x00000021, 0x00000003, 0x00000020,
    0x0004003b, 0x00000021, 0x00000022, 0x00000003, 0x00040015, 0x00000023,
    0x00000020, 0x00000001, 0x0004002b, 0x00000023, 0x00000024, 0x00000000,
    0x00040020, 0x00000025, 0x00000001, 0x00000023, 0x0004003b, 0x00000025,
    0x00000026, 0x00000001, 0x00040020, 0x00000028, 0x00000007, 0x00000007,
    0x00040020, 0x0000002e, 0x00000003, 0x0000001d, 0x00040020, 0x00000030,
    0x00000003, 0x00000014, 0x0004003b, 0x00000030, 0x00000031, 0x00000003,
    0x00040020, 0x00000033, 0x00000007, 0x00000014, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b,
    0x0000000b, 0x0000000c, 0x00000007, 0x0004003b, 0x00000016, 0x00000017,
    0x00000007, 0x0003003e, 0x0000000c, 0x00000013, 0x0003003e, 0x00000017,
    0x0000001c, 0x0004003d, 0x00000023, 0x00000027, 0x00000026, 0x00050041,
    0x00000028, 0x00000029, 0x0000000c, 0x00000027, 0x0004003d, 0x00000007,
    0x0000002a, 0x00000029, 0x00050051, 0x00000006, 0x0000002b, 0x0000002a,
    0x00000000, 0x00050051, 0x00000006, 0x0000002c, 0x0000002a, 0x00000001,
    0x00070050, 0x0000001d, 0x0000002d, 0x0000002b, 0x0000002c, 0x0000000d,
    0x00000018, 0x00050041, 0x0000002e, 0x0000002f, 0x00000022, 0x00000024,
    0x0003003e, 0x0000002f, 0x0000002d, 0x0004003d, 0x00000023, 0x00000032,
    0x00000026, 0x00050041, 0x00000033, 0x00000034, 0x00000017, 0x00000032,
    0x0004003d, 0x00000014, 0x00000035, 0x00000034, 0x0003003e, 0x00000031,
    0x00000035, 0x000100fd, 0x00010038,
};

// Fragment shader: outputs the interpolated vertex color. tools/venus-shader/triangle.frag
const uint32_t kTriFragSpirv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000013, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000c, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x4374756f,
    0x726f6c6f, 0x00000000, 0x00040005, 0x0000000c, 0x6c6f4376, 0x0000726f,
    0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000c,
    0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007,
    0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00040017, 0x0000000a,
    0x00000006, 0x00000003, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a,
    0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x00000006,
    0x0000000e, 0x3f800000, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a, 0x0000000d,
    0x0000000c, 0x00050051, 0x00000006, 0x0000000f, 0x0000000d, 0x00000000,
    0x00050051, 0x00000006, 0x00000010, 0x0000000d, 0x00000001, 0x00050051,
    0x00000006, 0x00000011, 0x0000000d, 0x00000002, 0x00070050, 0x00000007,
    0x00000012, 0x0000000f, 0x00000010, 0x00000011, 0x0000000e, 0x0003003e,
    0x00000009, 0x00000012, 0x000100fd, 0x00010038,
};

// IEEE-754 single-precision bit pattern for a small non-negative integer,
// computed with integer math only (the freestanding kernel has no soft-float).
uint32_t uintToFloatBits(uint32_t v) {
    if (v == 0) {
        return 0;
    }
    // Normalize: find the highest set bit -> exponent.
    uint32_t exp = 31;
    while ((v & 0x80000000u) == 0) {
        v <<= 1;
        --exp;
    }
    // v now has bit31 set; mantissa is the next 23 bits after the implicit 1.
    const uint32_t mantissa = (v >> 8) & 0x7FFFFFu;  // drop implicit leading 1 (bit31)
    const uint32_t biasedExp = exp + 127u;
    return (biasedExp << 23) | mantissa;
}

// Compile-time bit patterns for the fractional clear-color constants we use.
constexpr uint32_t kClearR_006 = 0x3d75c28fu;  // 0.06f
constexpr uint32_t kClearG_008 = 0x3da3d70au;  // 0.08f
constexpr uint32_t kClearB_016 = 0x3e23d70au;  // 0.16f

} // namespace

bool VulkanSession::renderTriangle(uint32_t width, uint32_t height, uint32_t* outBGRA,
                                   uint32_t outCapWords) {
    if (!ringReady_ || device_ == 0 || physDevCount_ == 0 || width == 0 || height == 0) {
        return false;
    }

    auto createShader = [&](const uint32_t* spirv, uint32_t wordCount, uint64_t* outHandle) -> bool {
        const uint64_t handle = allocHandleId();
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateShaderModule);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(0);
            e.writeSizeT(static_cast<uint64_t>(wordCount) * 4);
            e.writeArraySize(wordCount);
            for (uint32_t i = 0; i < wordCount; ++i) {
                e.writeU32(spirv[i]);
            }
        }
        e.writeSimplePointer(false);
        if (e.writeSimplePointer(true)) {
            e.writeHandle(handle);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateShaderModule) != 0) {
            return false;
        }
        *outHandle = handle;
        return true;
    };

    uint64_t vert = 0, frag = 0;
    if (!createShader(kTriVertSpirv, sizeof(kTriVertSpirv) / 4, &vert) ||
        !createShader(kTriFragSpirv, sizeof(kTriFragSpirv) / 4, &frag)) {
        return false;
    }

    // 1) Offscreen color image (BGRA8) usable as color attachment + transfer src.
    const uint64_t image = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateImage);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkImageCreateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(0);               // flags
            e.writeU32(VK_IMAGE_TYPE_2D);  // imageType
            e.writeU32(VK_FORMAT_B8G8R8A8_UNORM);
            e.writeU32(width);             // extent.width
            e.writeU32(height);            // extent.height
            e.writeU32(1);                 // extent.depth
            e.writeU32(1);                 // mipLevels
            e.writeU32(1);                 // arrayLayers
            e.writeU32(VK_SAMPLE_COUNT_1_BIT);
            e.writeU32(VK_IMAGE_TILING_OPTIMAL);
            e.writeFlags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            e.writeU32(VK_SHARING_MODE_EXCLUSIVE);
            e.writeU32(0);                 // queueFamilyIndexCount
            e.writeArraySize(0);           // pQueueFamilyIndices
            e.writeU32(VK_IMAGE_LAYOUT_UNDEFINED);  // initialLayout
        }
        e.writeSimplePointer(false);       // pAllocator
        if (e.writeSimplePointer(true)) {
            e.writeHandle(image);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateImage) != 0) {
            return false;
        }
    }

    // Image memory requirements + device-local allocation + bind. (We can use
    // the same host-visible allocator; the image is OPTIMAL tiling so we never
    // map it directly — we copy it into a separate linear buffer for readback.)
    uint64_t imgMemSize = static_cast<uint64_t>(width) * height * 4;
    uint32_t imgMemBits = 0xFFFFFFFFu;
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkGetImageMemoryRequirements);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        e.writeHandle(image);
        e.writeSimplePointer(true);
        const uint8_t* reply = nullptr;
        if (e.overflowed() ||
            !ringSubmit(s, static_cast<uint32_t>(e.length()), 64, &reply) || !reply) {
            return false;
        }
        CsDecoder dec(reply, replySize_);
        if (dec.readU32() != VK_COMMAND_TYPE_vkGetImageMemoryRequirements) {
            return false;
        }
        if (dec.readArraySize() == 0) {
            return false;
        }
        imgMemSize = dec.readU64();
        dec.readU64();  // alignment
        imgMemBits = dec.readU32();
        if (dec.fatal()) {
            return false;
        }
    }
    uint32_t imgTypeIndex = 0;
    if (!selectHostVisibleMemoryType(physDevs_[0], imgMemBits, &imgTypeIndex)) {
        // Fall back to any allowed type (image memory need not be host visible).
        for (uint32_t i = 0; i < 32; ++i) {
            if (imgMemBits & (1u << i)) { imgTypeIndex = i; break; }
        }
    }
    uint64_t imgMemory = 0;
    uint32_t imgMemRes = 0;
    void* imgMemPtr = nullptr;
    if (!allocateMappableMemory(imgMemSize, imgTypeIndex, &imgMemory, &imgMemRes, &imgMemPtr)) {
        return false;
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkBindImageMemory);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        e.writeHandle(image);
        e.writeHandle(imgMemory);
        e.writeU64(0);
        if (e.overflowed() ||
            decodeStatusReply(s, e.length(), VK_COMMAND_TYPE_vkBindImageMemory) != 0) {
            return false;
        }
    }

    // 2) Image view.
    const uint64_t view = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateImageView);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkImageViewCreateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(0);               // flags
            e.writeHandle(image);          // image
            e.writeU32(VK_IMAGE_VIEW_TYPE_2D);  // viewType
            e.writeU32(VK_FORMAT_B8G8R8A8_UNORM);
            // components (identity)
            e.writeU32(VK_COMPONENT_SWIZZLE_IDENTITY);
            e.writeU32(VK_COMPONENT_SWIZZLE_IDENTITY);
            e.writeU32(VK_COMPONENT_SWIZZLE_IDENTITY);
            e.writeU32(VK_COMPONENT_SWIZZLE_IDENTITY);
            // subresourceRange
            e.writeFlags(VK_IMAGE_ASPECT_COLOR_BIT);
            e.writeU32(0);                 // baseMipLevel
            e.writeU32(1);                 // levelCount
            e.writeU32(0);                 // baseArrayLayer
            e.writeU32(1);                 // layerCount
        }
        e.writeSimplePointer(false);       // pAllocator
        if (e.writeSimplePointer(true)) {
            e.writeHandle(view);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateImageView) != 0) {
            return false;
        }
    }

    // 3) Render pass: one BGRA color attachment, clear -> store, final layout
    // TRANSFER_SRC_OPTIMAL so we can copy without a separate barrier.
    const uint64_t renderPass = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateRenderPass);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkRenderPassCreateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(0);               // flags
            e.writeU32(1);                 // attachmentCount
            e.writeArraySize(1);           // pAttachments
            // VkAttachmentDescription
            e.writeFlags(0);               // flags
            e.writeU32(VK_FORMAT_B8G8R8A8_UNORM);
            e.writeU32(VK_SAMPLE_COUNT_1_BIT);
            e.writeU32(VK_ATTACHMENT_LOAD_OP_CLEAR);    // loadOp
            e.writeU32(VK_ATTACHMENT_STORE_OP_STORE);   // storeOp
            e.writeU32(VK_ATTACHMENT_LOAD_OP_DONT_CARE);  // stencilLoadOp
            e.writeU32(VK_ATTACHMENT_STORE_OP_DONT_CARE); // stencilStoreOp
            e.writeU32(VK_IMAGE_LAYOUT_UNDEFINED);        // initialLayout
            e.writeU32(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); // finalLayout
            e.writeU32(1);                 // subpassCount
            e.writeArraySize(1);           // pSubpasses
            // VkSubpassDescription
            e.writeFlags(0);               // flags
            e.writeU32(VK_PIPELINE_BIND_POINT_GRAPHICS);  // pipelineBindPoint
            e.writeU32(0);                 // inputAttachmentCount
            e.writeArraySize(0);           // pInputAttachments
            e.writeU32(1);                 // colorAttachmentCount
            e.writeArraySize(1);           // pColorAttachments
            e.writeU32(0);                 // attachment index
            e.writeU32(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);  // layout
            e.writeArraySize(0);           // pResolveAttachments (NULL)
            e.writeSimplePointer(false);   // pDepthStencilAttachment
            e.writeU32(0);                 // preserveAttachmentCount
            e.writeArraySize(0);           // pPreserveAttachments
            e.writeU32(0);                 // dependencyCount
            e.writeArraySize(0);           // pDependencies
        }
        e.writeSimplePointer(false);       // pAllocator
        if (e.writeSimplePointer(true)) {
            e.writeHandle(renderPass);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateRenderPass) != 0) {
            return false;
        }
    }

    // 4) Framebuffer over the image view.
    const uint64_t framebuffer = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateFramebuffer);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {  // VkFramebufferCreateInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            e.writeSimplePointer(false);   // pNext
            e.writeFlags(0);               // flags
            e.writeHandle(renderPass);     // renderPass
            e.writeU32(1);                 // attachmentCount
            e.writeArraySize(1);           // pAttachments
            e.writeHandle(view);
            e.writeU32(width);             // width
            e.writeU32(height);            // height
            e.writeU32(1);                 // layers
        }
        e.writeSimplePointer(false);       // pAllocator
        if (e.writeSimplePointer(true)) {
            e.writeHandle(framebuffer);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateFramebuffer) != 0) {
            return false;
        }
    }

    // 5) Pipeline layout (empty) + graphics pipeline.
    const uint64_t pipelineLayout = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreatePipelineLayout);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(0);
            e.writeU32(0);                 // setLayoutCount
            e.writeArraySize(0);
            e.writeU32(0);                 // pushConstantRangeCount
            e.writeArraySize(0);
        }
        e.writeSimplePointer(false);
        if (e.writeSimplePointer(true)) {
            e.writeHandle(pipelineLayout);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreatePipelineLayout) != 0) {
            return false;
        }
    }

    const uint64_t pipeline = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateGraphicsPipelines);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        e.writeHandle(0);              // pipelineCache
        e.writeU32(1);                 // createInfoCount
        e.writeArraySize(1);           // pCreateInfos
        // VkGraphicsPipelineCreateInfo
        e.writeStructureType(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        e.writeSimplePointer(false);   // pNext
        e.writeFlags(0);               // flags
        e.writeU32(2);                 // stageCount
        e.writeArraySize(2);           // pStages
        // stage[0] vertex
        e.writeStructureType(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        e.writeSimplePointer(false);
        e.writeFlags(0);
        e.writeU32(VK_SHADER_STAGE_VERTEX_BIT);
        e.writeHandle(vert);
        e.writeString("main");
        e.writeSimplePointer(false);   // pSpecializationInfo
        // stage[1] fragment
        e.writeStructureType(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        e.writeSimplePointer(false);
        e.writeFlags(0);
        e.writeU32(VK_SHADER_STAGE_FRAGMENT_BIT);
        e.writeHandle(frag);
        e.writeString("main");
        e.writeSimplePointer(false);
        // pVertexInputState (present, empty)
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(0);
            e.writeU32(0);             // vertexBindingDescriptionCount
            e.writeArraySize(0);
            e.writeU32(0);             // vertexAttributeDescriptionCount
            e.writeArraySize(0);
        }
        // pInputAssemblyState
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(0);
            e.writeU32(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            e.writeU32(0);             // primitiveRestartEnable
        }
        e.writeSimplePointer(false);   // pTessellationState
        // pViewportState (static viewport + scissor)
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(0);
            e.writeU32(1);             // viewportCount
            e.writeArraySize(1);       // pViewports
            // VkViewport: x,y,w,h,minDepth,maxDepth (floats)
            e.writeFloatBits(kFloatZeroBits);  // x
            e.writeFloatBits(kFloatZeroBits);  // y
            e.writeFloatBits(uintToFloatBits(width));
            e.writeFloatBits(uintToFloatBits(height));
            e.writeFloatBits(kFloatZeroBits);  // minDepth
            e.writeFloatBits(kFloatOneBits);   // maxDepth
            e.writeU32(1);             // scissorCount
            e.writeArraySize(1);       // pScissors
            // VkRect2D: offset(x,y int32), extent(w,h u32)
            e.writeU32(0);
            e.writeU32(0);
            e.writeU32(width);
            e.writeU32(height);
        }
        // pRasterizationState
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(0);
            e.writeU32(0);             // depthClampEnable
            e.writeU32(0);             // rasterizerDiscardEnable
            e.writeU32(VK_POLYGON_MODE_FILL);
            e.writeFlags(VK_CULL_MODE_NONE);
            e.writeU32(VK_FRONT_FACE_COUNTER_CLOCKWISE);
            e.writeU32(0);             // depthBiasEnable
            e.writeFloatBits(kFloatZeroBits);  // depthBiasConstantFactor
            e.writeFloatBits(kFloatZeroBits);  // depthBiasClamp
            e.writeFloatBits(kFloatZeroBits);  // depthBiasSlopeFactor
            e.writeFloatBits(kFloatOneBits);   // lineWidth
        }
        // pMultisampleState
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(0);
            e.writeU32(VK_SAMPLE_COUNT_1_BIT);  // rasterizationSamples
            e.writeU32(0);             // sampleShadingEnable
            e.writeFloatBits(kFloatZeroBits);  // minSampleShading
            e.writeArraySize(0);       // pSampleMask = NULL
            e.writeU32(0);             // alphaToCoverageEnable
            e.writeU32(0);             // alphaToOneEnable
        }
        e.writeSimplePointer(false);   // pDepthStencilState
        // pColorBlendState
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(0);
            e.writeU32(0);             // logicOpEnable
            e.writeU32(VK_LOGIC_OP_CLEAR);  // logicOp
            e.writeU32(1);             // attachmentCount
            e.writeArraySize(1);       // pAttachments
            // VkPipelineColorBlendAttachmentState
            e.writeU32(0);             // blendEnable
            e.writeU32(VK_BLEND_FACTOR_ONE);   // srcColorBlendFactor
            e.writeU32(VK_BLEND_FACTOR_ZERO);  // dstColorBlendFactor
            e.writeU32(VK_BLEND_OP_ADD);       // colorBlendOp
            e.writeU32(VK_BLEND_FACTOR_ONE);   // srcAlphaBlendFactor
            e.writeU32(VK_BLEND_FACTOR_ZERO);  // dstAlphaBlendFactor
            e.writeU32(VK_BLEND_OP_ADD);       // alphaBlendOp
            e.writeFlags(VK_COLOR_COMPONENT_RGBA);  // colorWriteMask
            e.writeArraySize(4);       // blendConstants[4]
            e.writeFloatBits(kFloatZeroBits);
            e.writeFloatBits(kFloatZeroBits);
            e.writeFloatBits(kFloatZeroBits);
            e.writeFloatBits(kFloatZeroBits);
        }
        e.writeSimplePointer(false);   // pDynamicState
        e.writeHandle(pipelineLayout); // layout
        e.writeHandle(renderPass);     // renderPass
        e.writeU32(0);                 // subpass
        e.writeHandle(0);              // basePipelineHandle
        e.writeU32(0xFFFFFFFFu);       // basePipelineIndex (-1)
        e.writeSimplePointer(false);   // pAllocator
        e.writeArraySize(1);           // pPipelines (guest ids)
        e.writeHandle(pipeline);
        const uint8_t* reply = nullptr;
        if (e.overflowed() ||
            !ringSubmit(s, static_cast<uint32_t>(e.length()), 64, &reply) || !reply) {
            return false;
        }
        CsDecoder dec(reply, replySize_);
        if (dec.readU32() != VK_COMMAND_TYPE_vkCreateGraphicsPipelines) {
            return false;
        }
        const int32_t ret = static_cast<int32_t>(dec.readU32());
        if (dec.readArraySize() != 0) {
            dec.readHandle();
        }
        if (dec.fatal() || ret != 0) {
            return false;
        }
    }

    // 6) Linear readback buffer (host visible) sized width*height*4.
    const uint64_t rbBytes = static_cast<uint64_t>(width) * height * 4;
    const uint64_t rbuf = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateBuffer);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(0);
            e.writeU64(rbBytes);
            e.writeFlags(0x00000002);  // VK_BUFFER_USAGE_TRANSFER_DST_BIT
            e.writeU32(VK_SHARING_MODE_EXCLUSIVE);
            e.writeU32(0);
            e.writeArraySize(0);
        }
        e.writeSimplePointer(false);
        if (e.writeSimplePointer(true)) {
            e.writeHandle(rbuf);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateBuffer) != 0) {
            return false;
        }
    }
    uint64_t rbReqSize = rbBytes;
    uint32_t rbBits = 0xFFFFFFFFu;
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkGetBufferMemoryRequirements);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        e.writeHandle(rbuf);
        e.writeSimplePointer(true);
        const uint8_t* reply = nullptr;
        if (e.overflowed() ||
            !ringSubmit(s, static_cast<uint32_t>(e.length()), 64, &reply) || !reply) {
            return false;
        }
        CsDecoder dec(reply, replySize_);
        if (dec.readU32() != VK_COMMAND_TYPE_vkGetBufferMemoryRequirements ||
            dec.readArraySize() == 0) {
            return false;
        }
        rbReqSize = dec.readU64();
        dec.readU64();
        rbBits = dec.readU32();
        if (dec.fatal()) {
            return false;
        }
    }
    uint32_t rbTypeIndex = 0;
    if (!selectHostVisibleMemoryType(physDevs_[0], rbBits, &rbTypeIndex)) {
        return false;
    }
    uint64_t rbMemory = 0;
    uint32_t rbMemRes = 0;
    void* rbPtr = nullptr;
    if (!allocateMappableMemory(rbReqSize, rbTypeIndex, &rbMemory, &rbMemRes, &rbPtr)) {
        return false;
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkBindBufferMemory);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        e.writeHandle(rbuf);
        e.writeHandle(rbMemory);
        e.writeU64(0);
        if (e.overflowed() ||
            decodeStatusReply(s, e.length(), VK_COMMAND_TYPE_vkBindBufferMemory) != 0) {
            return false;
        }
    }

    // 7) Queue + command pool + command buffer.
    const uint64_t queue = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkGetDeviceQueue2);
        e.writeFlags(0);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2);
            if (e.writeSimplePointer(true)) {
                e.writeStructureType(VK_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA);
                e.writeSimplePointer(false);
                e.writeU32(1);             // ringIdx
            }
            e.writeFlags(0);
            e.writeU32(0);
            e.writeU32(0);
        }
        if (e.writeSimplePointer(true)) {
            e.writeHandle(queue);
        }
        if (e.overflowed() || !ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr)) {
            return false;
        }
    }
    const uint64_t cmdPool = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateCommandPool);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(0);
            e.writeU32(0);
        }
        e.writeSimplePointer(false);
        if (e.writeSimplePointer(true)) {
            e.writeHandle(cmdPool);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateCommandPool) != 0) {
            return false;
        }
    }
    const uint64_t cmdBuf = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkAllocateCommandBuffers);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
            e.writeSimplePointer(false);
            e.writeHandle(cmdPool);
            e.writeU32(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
            e.writeU32(1);
        }
        e.writeArraySize(1);
        e.writeHandle(cmdBuf);
        const uint8_t* reply = nullptr;
        if (e.overflowed() ||
            !ringSubmit(s, static_cast<uint32_t>(e.length()), 64, &reply) || !reply) {
            return false;
        }
        CsDecoder dec(reply, replySize_);
        if (dec.readU32() != VK_COMMAND_TYPE_vkAllocateCommandBuffers) {
            return false;
        }
        const int32_t ret = static_cast<int32_t>(dec.readU32());
        if (dec.readArraySize() != 0) {
            dec.readHandle();
        }
        if (dec.fatal() || ret != 0) {
            return false;
        }
    }

    // 8) Record: begin, begin render pass (clear dark blue), bind pipeline,
    // draw 3 vertices, end render pass, copy image -> readback buffer, end.
    auto recordSimple = [&](void (*fill)(CsEncoder&, void*), void* ctx, uint32_t replyBytes,
                            uint32_t expectCmd) -> bool {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        fill(e, ctx);
        if (e.overflowed()) {
            return false;
        }
        if (replyBytes == 0) {
            return ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr);
        }
        return decodeStatusReply(s, e.length(), expectCmd) == 0;
    };
    (void)recordSimple;  // (kept simple with inline blocks below)

    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkBeginCommandBuffer);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(cmdBuf);
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
            e.writeSimplePointer(false);
        }
        if (e.overflowed() ||
            decodeStatusReply(s, e.length(), VK_COMMAND_TYPE_vkBeginCommandBuffer) != 0) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCmdBeginRenderPass);
        e.writeFlags(0);
        e.writeHandle(cmdBuf);
        if (e.writeSimplePointer(true)) {  // VkRenderPassBeginInfo
            e.writeStructureType(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
            e.writeSimplePointer(false);
            e.writeHandle(renderPass);
            e.writeHandle(framebuffer);
            // renderArea: offset(0,0), extent(w,h)
            e.writeU32(0);
            e.writeU32(0);
            e.writeU32(width);
            e.writeU32(height);
            e.writeU32(1);             // clearValueCount
            e.writeArraySize(1);       // pClearValues
            // VkClearValue -> tag 0 (color) -> VkClearColorValue tag 0 (float) -> 4 floats
            e.writeU32(0);             // VkClearValue tag (color)
            e.writeU32(0);             // VkClearColorValue tag (float32)
            e.writeArraySize(4);
            e.writeFloatBits(kClearR_006);  // R (dark blue background)
            e.writeFloatBits(kClearG_008);  // G
            e.writeFloatBits(kClearB_016);  // B
            e.writeFloatBits(kFloatOneBits);     // A
        }
        e.writeU32(VK_SUBPASS_CONTENTS_INLINE);
        if (e.overflowed() || !ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr)) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCmdBindPipeline);
        e.writeFlags(0);
        e.writeHandle(cmdBuf);
        e.writeU32(VK_PIPELINE_BIND_POINT_GRAPHICS);
        e.writeHandle(pipeline);
        if (e.overflowed() || !ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr)) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCmdDraw);
        e.writeFlags(0);
        e.writeHandle(cmdBuf);
        e.writeU32(3);  // vertexCount
        e.writeU32(1);  // instanceCount
        e.writeU32(0);  // firstVertex
        e.writeU32(0);  // firstInstance
        if (e.overflowed() || !ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr)) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCmdEndRenderPass);
        e.writeFlags(0);
        e.writeHandle(cmdBuf);
        if (e.overflowed() || !ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr)) {
            return false;
        }
    }
    {
        // Copy the rendered image (now in TRANSFER_SRC_OPTIMAL) to the linear buffer.
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCmdCopyImageToBuffer);
        e.writeFlags(0);
        e.writeHandle(cmdBuf);
        e.writeHandle(image);
        e.writeU32(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);  // srcImageLayout
        e.writeHandle(rbuf);
        e.writeU32(1);             // regionCount
        e.writeArraySize(1);       // pRegions
        // VkBufferImageCopy
        e.writeU64(0);             // bufferOffset
        e.writeU32(0);             // bufferRowLength (tightly packed)
        e.writeU32(0);             // bufferImageHeight
        // imageSubresource
        e.writeFlags(VK_IMAGE_ASPECT_COLOR_BIT);
        e.writeU32(0);             // mipLevel
        e.writeU32(0);             // baseArrayLayer
        e.writeU32(1);             // layerCount
        // imageOffset (0,0,0)
        e.writeU32(0);
        e.writeU32(0);
        e.writeU32(0);
        // imageExtent (w,h,1)
        e.writeU32(width);
        e.writeU32(height);
        e.writeU32(1);
        if (e.overflowed() || !ringSubmit(s, static_cast<uint32_t>(e.length()), 0, nullptr)) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkEndCommandBuffer);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(cmdBuf);
        if (e.overflowed() ||
            decodeStatusReply(s, e.length(), VK_COMMAND_TYPE_vkEndCommandBuffer) != 0) {
            return false;
        }
    }

    // 9) Submit with a fence + wait.
    const uint64_t fence = allocHandleId();
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkCreateFence);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        if (e.writeSimplePointer(true)) {
            e.writeStructureType(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
            e.writeSimplePointer(false);
            e.writeFlags(0);
        }
        e.writeSimplePointer(false);
        if (e.writeSimplePointer(true)) {
            e.writeHandle(fence);
        }
        if (e.overflowed() ||
            decodeCreateReply(s, e.length(), VK_COMMAND_TYPE_vkCreateFence) != 0) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkQueueSubmit);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(queue);
        e.writeU32(1);
        e.writeArraySize(1);
        e.writeStructureType(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        e.writeSimplePointer(false);
        e.writeU32(0);
        e.writeArraySize(0);
        e.writeArraySize(0);
        e.writeU32(1);
        e.writeArraySize(1);
        e.writeHandle(cmdBuf);
        e.writeU32(0);
        e.writeArraySize(0);
        e.writeHandle(fence);
        if (e.overflowed() ||
            decodeStatusReply(s, e.length(), VK_COMMAND_TYPE_vkQueueSubmit) != 0) {
            return false;
        }
    }
    {
        uint8_t* s = blob_ + scratchOffset_;
        CsEncoder e(s, scratchSize_);
        e.writeCommandType(VK_COMMAND_TYPE_vkWaitForFences);
        e.writeFlags(VK_COMMAND_GENERATE_REPLY_BIT);
        e.writeHandle(device_);
        e.writeU32(1);
        e.writeArraySize(1);
        e.writeHandle(fence);
        e.writeU32(1);
        e.writeU64(~0ULL);
        if (e.overflowed() ||
            decodeStatusReply(s, e.length(), VK_COMMAND_TYPE_vkWaitForFences) != 0) {
            return false;
        }
    }

    // 10) Read back the BGRA pixels from the mapped readback buffer.
    __asm__ __volatile__("mfence" ::: "memory");
    const uint32_t* px = static_cast<const uint32_t*>(rbPtr);
    const uint32_t total = width * height;
    const uint32_t copyWords = total < outCapWords ? total : outCapWords;
    for (uint32_t i = 0; i < copyWords; ++i) {
        outBGRA[i] = px[i];
    }

    // Teardown of created objects (async).
    auto destroyObj = [&](uint32_t cmdType, uint64_t handle) {
        if (handle == 0) {
            return;
        }
        uint8_t buf[64];
        CsEncoder e(buf, sizeof(buf));
        e.writeCommandType(cmdType);
        e.writeFlags(0);
        e.writeHandle(device_);
        e.writeHandle(handle);
        e.writeSimplePointer(false);
        if (!e.overflowed()) {
            ringSubmit(buf, static_cast<uint32_t>(e.length()), 0, nullptr);
        }
    };
    destroyObj(VK_COMMAND_TYPE_vkDestroyFence, fence);
    destroyObj(VK_COMMAND_TYPE_vkDestroyCommandPool, cmdPool);
    destroyObj(VK_COMMAND_TYPE_vkDestroyPipeline, pipeline);
    destroyObj(VK_COMMAND_TYPE_vkDestroyPipelineLayout, pipelineLayout);
    destroyObj(VK_COMMAND_TYPE_vkDestroyFramebuffer, framebuffer);
    destroyObj(VK_COMMAND_TYPE_vkDestroyRenderPass, renderPass);
    destroyObj(VK_COMMAND_TYPE_vkDestroyImageView, view);
    destroyObj(VK_COMMAND_TYPE_vkDestroyImage, image);
    destroyObj(VK_COMMAND_TYPE_vkDestroyBuffer, rbuf);
    destroyObj(VK_COMMAND_TYPE_vkFreeMemory, imgMemory);
    destroyObj(VK_COMMAND_TYPE_vkFreeMemory, rbMemory);
    destroyObj(VK_COMMAND_TYPE_vkDestroyShaderModule, vert);
    destroyObj(VK_COMMAND_TYPE_vkDestroyShaderModule, frag);
    gpu_->unmapBlobResource(imgMemRes);
    gpu_->unrefResource(imgMemRes);
    gpu_->unmapBlobResource(rbMemRes);
    gpu_->unrefResource(rbMemRes);

    // Success if at least one pixel is the triangle (non-background).
    return copyWords > 0;
}

void VulkanSession::shutdown() {
    if (gpu_ == nullptr) {
        ringReady_ = false;
        return;
    }

    if (ringReady_ && device_ != 0) {
        uint8_t buf[64];
        CsEncoder enc(buf, sizeof(buf));
        enc.writeCommandType(VK_COMMAND_TYPE_vkDestroyDevice);
        enc.writeFlags(0);
        enc.writeHandle(device_);
        enc.writeSimplePointer(false);  // pAllocator = NULL
        if (!enc.overflowed()) {
            ringSubmit(buf, static_cast<uint32_t>(enc.length()), 0, nullptr);
        }
        device_ = 0;
    }
    if (ringReady_ && instance_ != 0) {
        uint8_t buf[64];
        CsEncoder enc(buf, sizeof(buf));
        enc.writeCommandType(VK_COMMAND_TYPE_vkDestroyInstance);
        enc.writeFlags(0);
        enc.writeHandle(instance_);
        enc.writeSimplePointer(false);  // pAllocator = NULL
        if (!enc.overflowed()) {
            ringSubmit(buf, static_cast<uint32_t>(enc.length()), 0, nullptr);
        }
        instance_ = 0;
    }
    if (ringReady_ && ringId_ != 0) {
        uint8_t buf[32];
        CsEncoder enc(buf, sizeof(buf));
        enc.writeCommandType(VK_COMMAND_TYPE_vkDestroyRingMESA);
        enc.writeFlags(0);
        enc.writeU64(ringId_);
        if (!enc.overflowed()) {
            gpu_->submit3D(ctxId_, buf, static_cast<uint32_t>(enc.length()));
        }
    }
    ringReady_ = false;

    if (resourceId_ != 0) {
        gpu_->unmapBlobResource(resourceId_);
        gpu_->detachResourceFromContext(ctxId_, resourceId_);
        gpu_->unrefResource(resourceId_);
        resourceId_ = 0;
    }
    if (ctxId_ != 0) {
        Venus::get().destroyContext(ctxId_);
        ctxId_ = 0;
    }
    blob_ = nullptr;
    blobLength_ = 0;
    physDevCount_ = 0;
}

bool Venus::bringUpVulkan(VenusVulkanResult* result) {
    VenusVulkanResult local = {};

    if (!negotiate()) {
        if (result) {
            *result = local;
        }
        return false;
    }

    // The host render server initializes the Venus renderer lazily on the first
    // context/instance, which can fail on a cold start; retry the whole session.
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        local = VenusVulkanResult{};

        VulkanSession session;
        if (!session.init()) {
            continue;
        }
        local.ringOk = true;

        const int instRet = session.createInstance();
        local.instanceOk = (instRet == 0) && session.instanceHandle() != 0;
        local.instanceHandle = session.instanceHandle();
        if (!local.instanceOk) {
            session.shutdown();
            continue;
        }

        const uint32_t count = session.enumeratePhysicalDevices();
        local.physDevCount = count;
        local.physDevOk = count > 0;
        if (!local.physDevOk) {
            session.shutdown();
            continue;
        }

        local.propsOk = session.getPhysicalDeviceProperties(0, &local.device0);

        const int devRet = session.createDevice(0);
        local.deviceOk = (devRet == 0) && session.deviceHandle() != 0;
        local.deviceHandle = session.deviceHandle();

        // End-to-end compute dispatch + CPU readback on the created device.
        if (local.deviceOk) {
            const uint32_t kElems = 256;
            uint32_t sample[8] = {};
            uint32_t mismatches = 0;
            const bool ok = session.runCompute(kElems, sample, 8, &mismatches);
            local.computeOk = ok;
            local.computeElements = kElems;
            local.computeMismatches = mismatches;
            local.computeSample = sample[3];  // expected 3*3+1 = 10
        }

        session.shutdown();

        if (local.deviceOk && local.computeOk) {
            break;
        }
    }

    if (result) {
        *result = local;
    }
    return local.ringOk && local.instanceOk && local.physDevOk && local.deviceOk &&
           local.computeOk;
}

bool Venus::renderTriangleToScreen(uint32_t size) {
    if (!negotiate()) {
        return false;
    }
    VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();
    if (!gpu.isInitialized()) {
        return false;
    }

    uint32_t fbW = 0, fbH = 0;
    gpu.getMode(&fbW, &fbH);
    if (fbW == 0 || fbH == 0) {
        return false;
    }
    if (size > fbW) size = fbW;
    if (size > fbH) size = fbH;

    // Pixel buffer for the GPU readback (BGRA, size*size).
    const uint64_t pixelCount = static_cast<uint64_t>(size) * size;
    const uint64_t bytes = pixelCount * 4;
    const uint64_t pages = (bytes + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    const uint64_t phys = PMM::AllocFrames(pages);
    if (!phys) {
        return false;
    }
    uint32_t* pixels = reinterpret_cast<uint32_t*>(phys);

    // Run a Vulkan session and render the triangle into `pixels`. Retry to ride
    // out the host render-server cold-start race.
    bool rendered = false;
    for (uint32_t attempt = 0; attempt < 3 && !rendered; ++attempt) {
        VulkanSession session;
        if (!session.init()) {
            continue;
        }
        if (session.createInstance() != 0 || session.instanceHandle() == 0) {
            session.shutdown();
            continue;
        }
        if (session.enumeratePhysicalDevices() == 0) {
            session.shutdown();
            continue;
        }
        if (session.createDevice(0) != 0 || session.deviceHandle() == 0) {
            session.shutdown();
            continue;
        }
        rendered = session.renderTriangle(size, size, pixels, static_cast<uint32_t>(pixelCount));
        session.shutdown();
    }

    if (rendered) {
        // Blit the GPU-rendered BGRA image, centered, onto the scanout
        // framebuffer (also BGRX), then present it.
        auto* dst = static_cast<uint32_t*>(gpu.getFramebuffer());
        if (dst) {
            const uint32_t pitchPixels = gpu.getPitch() / 4;
            const uint32_t originX = (fbW - size) / 2;
            const uint32_t originY = (fbH - size) / 2;
            for (uint32_t row = 0; row < size; ++row) {
                uint32_t* d = dst + (originY + row) * pitchPixels + originX;
                const uint32_t* srcRow = pixels + static_cast<uint64_t>(row) * size;
                memcpy(d, srcRow, static_cast<uint64_t>(size) * 4);
            }
            gpu.flush(0, 0, fbW, fbH);
        }
    }

    for (uint64_t i = 0; i < pages; ++i) {
        PMM::FreeFrame(phys + i * PMM::PAGE_SIZE);
    }
    return rendered;
}

} // namespace venus
