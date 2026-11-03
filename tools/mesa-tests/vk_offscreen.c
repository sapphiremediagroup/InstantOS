// vk-offscreen: Vulkan through Mesa's anv in InstantOS, without a loader or a
// window system: the ICD (libvulkan_intel.so) opened as the Khronos loader
// would (vk_icdNegotiateLoaderICDInterfaceVersion, vk_icdGetInstanceProcAddr),
// an instance and a device, a 64x64 image cleared in a command buffer and
// copied into a host-visible buffer, read after a fence.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static int failures;

static void check(int ok, const char *what) {
    printf("vk-offscreen: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static PFN_vkGetInstanceProcAddr gipa;

// What anv says when something fails (vk_error reaches debug-utils messengers
// in every build), so a failure names its reason.
static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT types,
                                                 const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)types;
    (void)user;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        // Mesa puts the source location (file:line) in the id name.
        printf("vk-offscreen: anv says: %s (at %s)\n", data->pMessage,
               data->pMessageIdName ? data->pMessageIdName : "?");
    }
    return VK_FALSE;
}
static PFN_vkGetDeviceProcAddr gdpa;

#define INSTANCE_FN(instance, name) PFN_##name name = (PFN_##name)gipa(instance, #name)
#define DEVICE_FN(device, name) PFN_##name name = (PFN_##name)gdpa(device, #name)

static int finish(void) {
    printf(failures == 0 ? "vk-offscreen: all checks passed\n" : "vk-offscreen: some checks FAILED\n");
    return failures == 0 ? 0 : 1;
}

int main(void) {
    double t0 = now_ms();
    void *icd = dlopen("libvulkan_intel.so", RTLD_NOW | RTLD_LOCAL);
    check(icd != NULL, "dlopen libvulkan_intel.so");
    if (!icd) {
        printf("vk-offscreen: %s\n", dlerror());
        return finish();
    }
    VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr");
    uint32_t interface = 5;
    check(negotiate && negotiate(&interface) == VK_SUCCESS && gipa, "the ICD interface negotiated");
    if (!gipa) {
        return finish();
    }

    INSTANCE_FN(NULL, vkCreateInstance);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk-offscreen",
                             .apiVersion = VK_API_VERSION_1_3};
    VkDebugUtilsMessengerCreateInfoEXT messenger = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        .pfnUserCallback = on_message};
    const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstanceCreateInfo instanceInfo = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &messenger,
                                         .pApplicationInfo = &app, .enabledExtensionCount = 1,
                                         .ppEnabledExtensionNames = extensions};
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult created = vkCreateInstance(&instanceInfo, NULL, &instance);
    printf("vk-offscreen: vkCreateInstance %d after %.1f ms\n", created, now_ms() - t0);
    check(created == VK_SUCCESS, "vkCreateInstance");
    if (created != VK_SUCCESS) {
        return finish();
    }
    // A messenger in VkInstanceCreateInfo's chain only hears vkCreateInstance
    // and vkDestroyInstance; everything else needs a created one.
    INSTANCE_FN(instance, vkCreateDebugUtilsMessengerEXT);
    VkDebugUtilsMessengerEXT messengerHandle = VK_NULL_HANDLE;
    check(vkCreateDebugUtilsMessengerEXT &&
              vkCreateDebugUtilsMessengerEXT(instance, &messenger, NULL, &messengerHandle) == VK_SUCCESS,
          "a debug messenger");
    INSTANCE_FN(instance, vkEnumeratePhysicalDevices);
    INSTANCE_FN(instance, vkGetPhysicalDeviceProperties);
    INSTANCE_FN(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    INSTANCE_FN(instance, vkGetPhysicalDeviceMemoryProperties);
    INSTANCE_FN(instance, vkCreateDevice);
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");

    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    const VkResult enumerated = vkEnumeratePhysicalDevices(instance, &count, &physical);
    check((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) && count >= 1, "a physical device");
    if (count == 0 || physical == VK_NULL_HANDLE) {
        return finish();
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    printf("vk-offscreen: %s, Vulkan %u.%u.%u, driver 0x%x, vendor 0x%04x device 0x%04x\n", props.deviceName,
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
           VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, props.vendorID, props.deviceID);
    check(props.vendorID == 0x8086, "an Intel device");

    VkQueueFamilyProperties families[8];
    uint32_t familyCount = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families);
    uint32_t family = 0;
    while (family < familyCount && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        family++;
    }
    check(family < familyCount, "a graphics queue family");
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                         .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo deviceInfo = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                     .pQueueCreateInfos = &queueInfo};
    VkDevice device = VK_NULL_HANDLE;
    const VkResult deviceResult = vkCreateDevice(physical, &deviceInfo, NULL, &device);
    printf("vk-offscreen: vkCreateDevice returned %d\n", deviceResult);
    check(deviceResult == VK_SUCCESS, "vkCreateDevice");
    if (device == VK_NULL_HANDLE) {
        return finish();
    }
    DEVICE_FN(device, vkGetDeviceQueue);
    DEVICE_FN(device, vkCreateImage);
    DEVICE_FN(device, vkGetImageMemoryRequirements);
    DEVICE_FN(device, vkAllocateMemory);
    DEVICE_FN(device, vkBindImageMemory);
    DEVICE_FN(device, vkCreateBuffer);
    DEVICE_FN(device, vkGetBufferMemoryRequirements);
    DEVICE_FN(device, vkBindBufferMemory);
    DEVICE_FN(device, vkMapMemory);
    DEVICE_FN(device, vkCreateCommandPool);
    DEVICE_FN(device, vkAllocateCommandBuffers);
    DEVICE_FN(device, vkBeginCommandBuffer);
    DEVICE_FN(device, vkEndCommandBuffer);
    DEVICE_FN(device, vkCmdPipelineBarrier);
    DEVICE_FN(device, vkCmdClearColorImage);
    DEVICE_FN(device, vkCmdCopyImageToBuffer);
    DEVICE_FN(device, vkCreateFence);
    DEVICE_FN(device, vkQueueSubmit);
    DEVICE_FN(device, vkWaitForFences);
    DEVICE_FN(device, vkDeviceWaitIdle);
    DEVICE_FN(device, vkDestroyDevice);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    // The first type with the wanted properties among those allowed.
    #define MEMORY_TYPE(bits, wanted)                                                                     \
        ({                                                                                                \
            uint32_t t = 0;                                                                               \
            while (t < memory.memoryTypeCount &&                                                          \
                   !(((bits) & (1u << t)) && (memory.memoryTypes[t].propertyFlags & (wanted)) == (wanted))) \
                t++;                                                                                      \
            t;                                                                                            \
        })

    VkImageCreateInfo imageInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                   .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {64, 64, 1}, .mipLevels = 1,
                                   .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                   .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image = VK_NULL_HANDLE;
    VkMemoryRequirements imageReq;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    int ok = vkCreateImage(device, &imageInfo, NULL, &image) == VK_SUCCESS;
    if (ok) {
        vkGetImageMemoryRequirements(device, image, &imageReq);
        VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = imageReq.size,
                                      .memoryTypeIndex = MEMORY_TYPE(imageReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        ok = vkAllocateMemory(device, &alloc, NULL, &imageMemory) == VK_SUCCESS &&
             vkBindImageMemory(device, image, imageMemory, 0) == VK_SUCCESS;
    }
    check(ok, "a 64x64 R8G8B8A8 image with its memory");

    VkBufferCreateInfo bufferInfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64 * 64 * 4,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    unsigned char *mapped = NULL;
    ok = vkCreateBuffer(device, &bufferInfo, NULL, &buffer) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buffer, &req);
        VkMemoryAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = MEMORY_TYPE(req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        ok = vkAllocateMemory(device, &alloc, NULL, &bufferMemory) == VK_SUCCESS &&
             vkBindBufferMemory(device, buffer, bufferMemory, 0) == VK_SUCCESS &&
             vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped) == VK_SUCCESS;
    }
    check(ok && mapped, "a host-visible buffer, mapped");
    if (!ok || !mapped) {
        return finish();
    }
    memset(mapped, 0, 64 * 64 * 4);

    VkCommandPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &poolInfo, NULL, &pool);
    VkCommandBufferAllocateInfo cmdInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device, &cmdInfo, &cmd) == VK_SUCCESS, "a command buffer");
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &begin);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier toDst = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = image, .subresourceRange = range};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst);
    VkClearColorValue color = {.float32 = {0.25f, 0.5f, 0.75f, 1.0f}};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    VkImageMemoryBarrier toSrc = toDst;
    toSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toSrc);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {64, 64, 1}};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    VkBufferMemoryBarrier toHost = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = buffer, .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &toHost, 0, NULL);
    check(vkEndCommandBuffer(cmd) == VK_SUCCESS, "the commands recorded (barriers, a clear, a copy)");

    VkFenceCreateInfo fenceInfo = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fenceInfo, NULL, &fence);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    double t1 = now_ms();
    const VkResult submitted = vkQueueSubmit(queue, 1, &submit, fence);
    const VkResult waited = submitted == VK_SUCCESS ? vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull) : submitted;
    printf("vk-offscreen: submitted and fenced in %.2f ms (submit %d, wait %d)\n", now_ms() - t1, submitted, waited);
    check(submitted == VK_SUCCESS && waited == VK_SUCCESS, "vkQueueSubmit, then the fence signals");
    const unsigned char *first = mapped;
    const unsigned char *last = mapped + (64 * 64 - 1) * 4;
    printf("vk-offscreen: first pixel %u,%u,%u,%u last %u,%u,%u,%u\n", first[0], first[1], first[2], first[3], last[0],
           last[1], last[2], last[3]);
    check(abs(first[0] - 64) <= 1 && abs(first[1] - 128) <= 1 && abs(first[2] - 191) <= 1 && first[3] == 255 &&
              memcmp(first, last, 4) == 0,
          "the image was cleared to (64,128,191,255) and copied out");
    vkDeviceWaitIdle(device);
    vkDestroyDevice(device, NULL);
    return finish();
}
