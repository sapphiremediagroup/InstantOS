// vk-window: Vulkan in a compositor window through Mesa's anv and the
// InstantOS WSI (VK_INSTANTOS_surface, step 7c-2c). The ICD opened as the
// Khronos loader would (as vk-offscreen does), a surface on the window, a
// FIFO swapchain, and gl-window's spinning cube drawn with a depth buffer,
// presented with vkQueuePresentKHR. The swapchain is made again when the
// window is resized; Escape or the close button ends it.
//
//   vk-window [frames]    0 or nothing: until closed
//
// Prints checks (the first frame copied back before its present), then the
// frame rate every 2 s and at the end.
#include <dlfcn.h>
#include <instantos/window.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_INSTANTOS
#include <vulkan/vulkan.h>

#include "vk_window_cube.frag.h"
#include "vk_window_cube.vert.h"

static int failures;

static void check(int ok, const char *what) {
    printf("vk-window: %s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (!ok) {
        failures++;
    }
}

static int finish(void) {
    printf(failures == 0 ? "vk-window: all checks passed\n" : "vk-window: some checks FAILED\n");
    return failures == 0 ? 0 : 1;
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT types,
                                                 const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)types;
    (void)user;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        printf("vk-window: anv says: %s (at %s)\n", data->pMessage, data->pMessageIdName ? data->pMessageIdName : "?");
    }
    return VK_FALSE;
}

// The entry points, loaded through vk_icdGetInstanceProcAddr and vkGetDeviceProcAddr.
#define INSTANCE_FNS(X)                                                                                              \
    X(vkCreateDebugUtilsMessengerEXT) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                 \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice)              \
    X(vkGetDeviceProcAddr) X(vkCreateInstantOSSurfaceINSTANTOS) X(vkGetPhysicalDeviceSurfaceSupportKHR)              \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)                              \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkDestroySurfaceKHR) X(vkDestroyInstance)
#define DEVICE_FNS(X)                                                                                                \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR)                  \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateImage)        \
    X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory)       \
    X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateRenderPass)    \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkCreatePipelineLayout)                 \
    X(vkCreateGraphicsPipelines) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)          \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)                      \
    X(vkCmdBindPipeline) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw)                   \
    X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer) X(vkCreateSemaphore) X(vkCreateFence) X(vkWaitForFences)       \
    X(vkResetFences) X(vkQueueSubmit) X(vkDeviceWaitIdle) X(vkDestroyDevice)
#define DECLARE(name) static PFN_##name name;
INSTANCE_FNS(DECLARE)
DEVICE_FNS(DECLARE)

static VkPhysicalDevice physical;
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory;
static VkSurfaceKHR surface;
static VkRenderPass renderPass;
static const VkFormat kColourFormat = VK_FORMAT_B8G8R8A8_UNORM;   // gl-window's colours, unconverted
static const VkFormat kDepthFormat = VK_FORMAT_D16_UNORM;

// What depends on the window's size.
#define MAX_IMAGES 8
static struct {
    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    uint32_t count;
    VkImage images[MAX_IMAGES];
    VkImageView views[MAX_IMAGES];
    VkFramebuffer framebuffers[MAX_IMAGES];
    VkImage depth;
    VkDeviceMemory depthMemory;
    VkImageView depthView;
} chain;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted) {
    for (uint32_t t = 0; t < memory.memoryTypeCount; t++) {
        if ((bits & (1u << t)) && (memory.memoryTypes[t].propertyFlags & wanted) == wanted) {
            return t;
        }
    }
    return 0;
}

static void destroy_chain_parts(void) {
    for (uint32_t i = 0; i < chain.count; i++) {
        vkDestroyFramebuffer(device, chain.framebuffers[i], NULL);
        vkDestroyImageView(device, chain.views[i], NULL);
    }
    if (chain.depthView) {
        vkDestroyImageView(device, chain.depthView, NULL);
        vkDestroyImage(device, chain.depth, NULL);
        vkFreeMemory(device, chain.depthMemory, NULL);
    }
    chain.depthView = VK_NULL_HANDLE;
    chain.count = 0;
}

// A swapchain of width x height (replacing the old one), its views, a depth
// buffer and the framebuffers.
static int make_chain(uint32_t width, uint32_t height, int report) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps) != VK_SUCCESS) {
        return 0;
    }
    if (report) {
        printf("vk-window: surface: %u-%u images, current extent %ux%u, max %ux%u\n", caps.minImageCount,
               caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height, caps.maxImageExtent.width,
               caps.maxImageExtent.height);
        check(caps.currentExtent.width == UINT32_MAX, "the extent is the swapchain's to choose (the window's size)");
    }
    vkDeviceWaitIdle(device);
    destroy_chain_parts();
    VkSwapchainKHR old = chain.swapchain;
    VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = caps.minImageCount,
        .imageFormat = kColourFormat, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {width, height}, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE, .oldSwapchain = old};
    const VkResult made = vkCreateSwapchainKHR(device, &info, NULL, &chain.swapchain);
    if (old) {
        vkDestroySwapchainKHR(device, old, NULL);
    }
    if (made != VK_SUCCESS) {
        printf("vk-window: vkCreateSwapchainKHR %d\n", made);
        chain.swapchain = VK_NULL_HANDLE;
        return 0;
    }
    chain.extent = info.imageExtent;
    chain.count = MAX_IMAGES;
    vkGetSwapchainImagesKHR(device, chain.swapchain, &chain.count, chain.images);
    if (report) {
        printf("vk-window: swapchain of %u images at %ux%u\n", chain.count, width, height);
    }

    VkImageCreateInfo depthInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                   .format = kDepthFormat, .extent = {width, height, 1}, .mipLevels = 1,
                                   .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .tiling = VK_IMAGE_TILING_OPTIMAL,
                                   .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(device, &depthInfo, NULL, &chain.depth) != VK_SUCCESS) {
        return 0;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, chain.depth, &req);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                  .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(device, &alloc, NULL, &chain.depthMemory) != VK_SUCCESS ||
        vkBindImageMemory(device, chain.depth, chain.depthMemory, 0) != VK_SUCCESS) {
        return 0;
    }
    VkImageViewCreateInfo depthView = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = chain.depth,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = kDepthFormat,
                                       .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(device, &depthView, NULL, &chain.depthView) != VK_SUCCESS) {
        return 0;
    }
    for (uint32_t i = 0; i < chain.count; i++) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = chain.images[i],
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = kColourFormat,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(device, &view, NULL, &chain.views[i]) != VK_SUCCESS) {
            return 0;
        }
        VkImageView attachments[2] = {chain.views[i], chain.depthView};
        VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = renderPass,
                                      .attachmentCount = 2, .pAttachments = attachments, .width = width,
                                      .height = height, .layers = 1};
        if (vkCreateFramebuffer(device, &fb, NULL, &chain.framebuffers[i]) != VK_SUCCESS) {
            return 0;
        }
    }
    return 1;
}

static void multiply(float *out, const float *a, const float *b) {
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            r[c * 4 + row] = 0;
            for (int k = 0; k < 4; k++) {
                r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
            }
        }
    }
    memcpy(out, r, sizeof(r));
}

// gl-window's transform, with Vulkan's clip space: y down, depth 0..1.
static void transform(float *out, float t, float aspect) {
    const float f = 1.0f / tanf(0.5f * 0.9f);
    const float nearZ = 1.0f, farZ = 20.0f;
    float projection[16] = {f / aspect, 0, 0, 0, 0, -f, 0, 0, 0, 0, farZ / (nearZ - farZ), -1,
                            0, 0, farZ * nearZ / (nearZ - farZ), 0};
    float move[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -6, 1};
    const float a = t * 0.9f, b = t * 0.6f;
    float spinY[16] = {cosf(a), 0, -sinf(a), 0, 0, 1, 0, 0, sinf(a), 0, cosf(a), 0, 0, 0, 0, 1};
    float spinX[16] = {1, 0, 0, 0, 0, cosf(b), sinf(b), 0, 0, -sinf(b), cosf(b), 0, 0, 0, 0, 1};
    float m[16];
    multiply(m, spinY, spinX);
    multiply(m, move, m);
    multiply(out, projection, m);
}

int main(int argc, char **argv) {
    long frames = argc > 1 ? strtol(argv[1], NULL, 10) : 0;
    uint32_t width = 640, height = 480;

    struct instantos_window window;
    check(instantos_window_create(&window, (int)width, (int)height, "Vulkan cube") == 0, "a compositor window");
    if (failures) {
        return finish();
    }

    void *icd = dlopen("libvulkan_intel.so", RTLD_NOW | RTLD_LOCAL);
    VkResult (*negotiate)(uint32_t *) =
        icd ? (VkResult (*)(uint32_t *))dlsym(icd, "vk_icdNegotiateLoaderICDInterfaceVersion") : NULL;
    PFN_vkGetInstanceProcAddr gipa = icd ? (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr") : NULL;
    uint32_t interface = 5;
    check(negotiate && negotiate(&interface) == VK_SUCCESS && gipa, "the ICD (libvulkan_intel.so) loaded");
    if (failures) {
        return finish();
    }

    PFN_vkEnumerateInstanceExtensionProperties enumerateExtensions =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    VkExtensionProperties extensionList[64];
    uint32_t extensionCount = 64;
    enumerateExtensions(NULL, &extensionCount, extensionList);
    int hasSurface = 0;
    for (uint32_t i = 0; i < extensionCount; i++) {
        hasSurface |= strcmp(extensionList[i].extensionName, VK_INSTANTOS_SURFACE_EXTENSION_NAME) == 0;
    }
    check(hasSurface, "the instance offers " VK_INSTANTOS_SURFACE_EXTENSION_NAME);

    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk-window",
                             .apiVersion = VK_API_VERSION_1_3};
    VkDebugUtilsMessengerCreateInfoEXT messenger = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        .pfnUserCallback = on_message};
    const char *instanceExtensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_KHR_SURFACE_EXTENSION_NAME,
                                        VK_INSTANTOS_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo instanceInfo = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &messenger,
                                         .pApplicationInfo = &app, .enabledExtensionCount = 3,
                                         .ppEnabledExtensionNames = instanceExtensions};
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult created = vkCreateInstance(&instanceInfo, NULL, &instance);
    check(created == VK_SUCCESS, "vkCreateInstance with VK_KHR_surface and " VK_INSTANTOS_SURFACE_EXTENSION_NAME);
    if (created != VK_SUCCESS) {
        printf("vk-window: vkCreateInstance %d\n", created);
        return finish();
    }
#define LOAD_INSTANCE(name) name = (PFN_##name)gipa(instance, #name);
    INSTANCE_FNS(LOAD_INSTANCE)
    VkDebugUtilsMessengerEXT messengerHandle = VK_NULL_HANDLE;
    vkCreateDebugUtilsMessengerEXT(instance, &messenger, NULL, &messengerHandle);
    check(vkCreateInstantOSSurfaceINSTANTOS != NULL, "vkCreateInstantOSSurfaceINSTANTOS resolves");
    if (failures) {
        return finish();
    }

    uint32_t count = 1;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    check(count >= 1 && physical != VK_NULL_HANDLE, "a physical device");
    if (failures) {
        return finish();
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    printf("vk-window: %s, Vulkan %u.%u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    VkInstantOSSurfaceCreateInfoINSTANTOS surfaceInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANTOS_SURFACE_CREATE_INFO_INSTANTOS, .window = window.window};
    check(vkCreateInstantOSSurfaceINSTANTOS(instance, &surfaceInfo, NULL, &surface) == VK_SUCCESS,
          "a surface on the window");

    VkQueueFamilyProperties families[8];
    uint32_t familyCount = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families);
    uint32_t family = 0;
    VkBool32 presents = VK_FALSE;
    while (family < familyCount) {
        presents = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &presents);
        if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presents) {
            break;
        }
        family++;
    }
    check(family < familyCount, "a graphics queue that can present to it");

    VkSurfaceFormatKHR formats[16];
    uint32_t formatCount = 16;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats);
    int hasFormat = 0;
    for (uint32_t i = 0; i < formatCount; i++) {
        hasFormat |= formats[i].format == kColourFormat;
    }
    check(hasFormat, "B8G8R8A8_UNORM among the surface's formats");
    VkPresentModeKHR modes[8];
    uint32_t modeCount = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, modes);
    int hasFifo = 0;
    for (uint32_t i = 0; i < modeCount; i++) {
        hasFifo |= modes[i] == VK_PRESENT_MODE_FIFO_KHR;
    }
    check(hasFifo, "FIFO presentation");
    if (failures) {
        return finish();
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                         .queueCount = 1, .pQueuePriorities = &priority};
    const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                     .pQueueCreateInfos = &queueInfo, .enabledExtensionCount = 1,
                                     .ppEnabledExtensionNames = deviceExtensions};
    const VkResult deviceResult = vkCreateDevice(physical, &deviceInfo, NULL, &device);
    check(deviceResult == VK_SUCCESS, "vkCreateDevice with VK_KHR_swapchain");
    if (deviceResult != VK_SUCCESS) {
        printf("vk-window: vkCreateDevice %d\n", deviceResult);
        return finish();
    }
#define LOAD_DEVICE(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name);
    DEVICE_FNS(LOAD_DEVICE)
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    // The render pass: the cube over the window colour, with depth; the
    // image is left ready to present.
    VkAttachmentDescription attachments[2] = {
        {.format = kColourFormat, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
        {.format = kDepthFormat, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
    };
    VkAttachmentReference colourRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                    .pColorAttachments = &colourRef, .pDepthStencilAttachment = &depthRef};
    // The acquire semaphore is waited for at colour output: the image's
    // layout change must come after it.
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo passInfo = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2,
                                       .pAttachments = attachments, .subpassCount = 1, .pSubpasses = &subpass,
                                       .dependencyCount = 1, .pDependencies = &dependency};
    check(vkCreateRenderPass(device, &passInfo, NULL, &renderPass) == VK_SUCCESS, "a render pass with depth");

    VkShaderModuleCreateInfo vertInfo = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                         .codeSize = sizeof(kCubeVert), .pCode = kCubeVert};
    VkShaderModuleCreateInfo fragInfo = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                         .codeSize = sizeof(kCubeFrag), .pCode = kCubeFrag};
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &vertInfo, NULL, &vert);
    vkCreateShaderModule(device, &fragInfo, NULL, &frag);
    VkPushConstantRange pushRange = {VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
    VkPipelineLayoutCreateInfo layoutInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                             .pushConstantRangeCount = 1, .pPushConstantRanges = &pushRange};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &layoutInfo, NULL, &layout);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                       .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewportState = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                       .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL,
                                                     .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depthState = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                                        .depthCompareOp = VK_COMPARE_OP_LESS};
    VkPipelineColorBlendAttachmentState blendAttachment = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                 .attachmentCount = 1, .pAttachments = &blendAttachment};
    VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = 2, .pDynamicStates = dynamics};
    VkGraphicsPipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vertexInput, .pInputAssemblyState = &assembly, .pViewportState = &viewportState,
        .pRasterizationState = &raster, .pMultisampleState = &multisample, .pDepthStencilState = &depthState,
        .pColorBlendState = &blend, .pDynamicState = &dynamic, .layout = layout, .renderPass = renderPass};
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vert && frag && layout &&
              vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline) == VK_SUCCESS,
          "the cube's pipeline");

    check(make_chain(width, height, 1), "a FIFO swapchain on the surface");
    if (failures) {
        return finish();
    }

    // Two pixels of the first frame (corner, middle) are copied out here.
    VkBufferCreateInfo bufferInfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 8,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    unsigned char *pixels = NULL;
    vkCreateBuffer(device, &bufferInfo, NULL, &readback);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, readback, &req);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkAllocateMemory(device, &alloc, NULL, &readbackMemory);
    vkBindBufferMemory(device, readback, readbackMemory, 0);
    vkMapMemory(device, readbackMemory, 0, VK_WHOLE_SIZE, 0, (void **)&pixels);
    memset(pixels, 0, 8);

    VkCommandPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                        .queueFamilyIndex = family};
    VkCommandPool pool;
    vkCreateCommandPool(device, &poolInfo, NULL, &pool);
    VkCommandBufferAllocateInfo cmdInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cmdInfo, &cmd);
    VkSemaphoreCreateInfo semInfo = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired, rendered;
    vkCreateSemaphore(device, &semInfo, NULL, &acquired);
    vkCreateSemaphore(device, &semInfo, NULL, &rendered);
    VkFenceCreateInfo fenceInfo = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence done;
    vkCreateFence(device, &fenceInfo, NULL, &done);

    const double start = now_s();
    double lastReport = start;
    long drawn = 0, sinceReport = 0;
    int running = 1;
    uint32_t wantWidth = width, wantHeight = height;
    while (running && (frames <= 0 || drawn < frames)) {
        struct instantos_event event;
        while (instantos_window_next_event(&window, &event, 0)) {
            if (event.type == INSTANTOS_EVENT_WINDOW && event.window.action == INSTANTOS_WINDOW_CLOSE_REQUESTED) {
                running = 0;
            } else if (event.type == INSTANTOS_EVENT_WINDOW && event.window.action == INSTANTOS_WINDOW_RESIZED &&
                       event.window.width > 0 && event.window.height > 0) {
                wantWidth = (uint32_t)event.window.width;
                wantHeight = (uint32_t)event.window.height;
            } else if (event.type == INSTANTOS_EVENT_KEY && event.key.action == INSTANTOS_KEY_PRESS &&
                       event.key.keycode == INSTANTOS_KEY_ESCAPE) {
                running = 0;
            }
        }
        if (!running) {
            break;
        }
        if (wantWidth != chain.extent.width || wantHeight != chain.extent.height) {
            if (!make_chain(wantWidth, wantHeight, 0)) {
                check(0, "the swapchain made again at the window's new size");
                break;
            }
            printf("vk-window: swapchain made again at %ux%u\n", wantWidth, wantHeight);
        }

        vkWaitForFences(device, 1, &done, VK_TRUE, UINT64_MAX);
        uint32_t index = 0;
        const VkResult got = vkAcquireNextImageKHR(device, chain.swapchain, 2000000000ull, acquired, VK_NULL_HANDLE, &index);
        if (got != VK_SUCCESS && got != VK_SUBOPTIMAL_KHR) {
            printf("vk-window: vkAcquireNextImageKHR %d\n", got);
            failures++;
            break;
        }
        vkResetFences(device, 1, &done);
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cmd, &begin);
        VkClearValue clears[2] = {{.color = {.float32 = {0.086f, 0.114f, 0.157f, 1.0f}}},   // #161d28
                                  {.depthStencil = {1.0f, 0}}};
        VkRenderPassBeginInfo passBegin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = renderPass,
                                           .framebuffer = chain.framebuffers[index],
                                           .renderArea = {{0, 0}, chain.extent}, .clearValueCount = 2,
                                           .pClearValues = clears};
        vkCmdBeginRenderPass(cmd, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport viewport = {0, 0, (float)chain.extent.width, (float)chain.extent.height, 0, 1};
        VkRect2D scissor = {{0, 0}, chain.extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        float m[16];
        transform(m, (float)(now_s() - start), (float)chain.extent.width / (float)chain.extent.height);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(m), m);
        vkCmdDraw(cmd, 36, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        if (drawn == 0) {
            // The corner and the middle, out to the buffer, then back to present.
            VkImageMemoryBarrier toCopy = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = chain.images[index],
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, NULL, 0, NULL, 1, &toCopy);
            VkBufferImageCopy regions[2] = {
                {.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                 .imageOffset = {2, 2, 0}, .imageExtent = {1, 1, 1}},
                {.bufferOffset = 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                 .imageOffset = {(int32_t)chain.extent.width / 2, (int32_t)chain.extent.height / 2, 0},
                 .imageExtent = {1, 1, 1}},
            };
            vkCmdCopyImageToBuffer(cmd, chain.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 2, regions);
            VkImageMemoryBarrier toPresent = toCopy;
            toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toPresent.dstAccessMask = 0;
            toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                                 0, NULL, 1, &toPresent);
        }
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &acquired, .pWaitDstStageMask = &waitStage,
                               .commandBufferCount = 1, .pCommandBuffers = &cmd, .signalSemaphoreCount = 1,
                               .pSignalSemaphores = &rendered};
        if (vkQueueSubmit(queue, 1, &submit, done) != VK_SUCCESS) {
            check(0, "vkQueueSubmit");
            break;
        }
        if (drawn == 0) {
            vkWaitForFences(device, 1, &done, VK_TRUE, UINT64_MAX);
            // BGRA in memory.
            printf("vk-window: first frame: corner %d,%d,%d middle %d,%d,%d\n", pixels[2], pixels[1], pixels[0],
                   pixels[6], pixels[5], pixels[4]);
            check(abs(pixels[2] - 22) <= 2 && abs(pixels[1] - 29) <= 2 && abs(pixels[0] - 40) <= 2,
                  "the first frame's corner is the clear colour");
            check(!(abs(pixels[6] - 22) <= 2 && abs(pixels[5] - 29) <= 2 && abs(pixels[4] - 40) <= 2),
                  "and the cube covers its middle");
        }
        VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                    .pWaitSemaphores = &rendered, .swapchainCount = 1,
                                    .pSwapchains = &chain.swapchain, .pImageIndices = &index};
        const VkResult shown = vkQueuePresentKHR(queue, &present);
        if (shown != VK_SUCCESS && shown != VK_SUBOPTIMAL_KHR) {
            printf("vk-window: vkQueuePresentKHR %d\n", shown);
            failures++;
            break;
        }
        if (drawn == 0) {
            check(1, "vkQueuePresentKHR shows the first frame");
        }
        drawn++;
        sinceReport++;
        const double t = now_s();
        if (t - lastReport >= 2.0) {
            printf("vk-window: %.1f frames per second at %ux%u\n", sinceReport / (t - lastReport), chain.extent.width,
                   chain.extent.height);
            fflush(stdout);
            lastReport = t;
            sinceReport = 0;
        }
    }
    const double elapsed = now_s() - start;
    printf("vk-window: %ld frames in %.2f s (%.1f per second)\n", drawn, elapsed, elapsed > 0 ? drawn / elapsed : 0.0);

    vkDeviceWaitIdle(device);
    destroy_chain_parts();
    vkDestroySwapchainKHR(device, chain.swapchain, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
    instantos_window_destroy(&window);
    return finish();
}
