#ifndef VULKAN_INSTANTOS_H_
#define VULKAN_INSTANTOS_H_ 1

/*
 * VK_INSTANTOS_surface: a Vulkan surface on an InstantOS compositor window
 * (the window handle from instantos_window_create, <instantos/window.h>).
 * InstantOS's own platform, not a Khronos one: it is declared in Mesa's
 * vk.xml (tools/mesa-port/patch-mesa.py) so the generated dispatch knows it,
 * and enabled with VK_USE_PLATFORM_INSTANTOS.
 *
 * The surface has no fixed size: currentExtent is 0xFFFFFFFF and the
 * swapchain's extent is the size the window shows (resize the window, then
 * recreate the swapchain). Images are linear BGRA/RGBA dma-bufs the
 * compositor maps; presenting commits one once the GPU has drawn it, and an
 * image is acquired again once the compositor let it go (FIFO, paced by the
 * compositor's flips).
 */

#ifdef __cplusplus
extern "C" {
#endif

#define VK_INSTANTOS_surface 1
#define VK_INSTANTOS_SURFACE_SPEC_VERSION 1
#define VK_INSTANTOS_SURFACE_EXTENSION_NAME "VK_INSTANTOS_surface"
/* Extension number 999 (outside the Khronos registry's range). */
#define VK_STRUCTURE_TYPE_INSTANTOS_SURFACE_CREATE_INFO_INSTANTOS ((VkStructureType)1000998000)
typedef VkFlags VkInstantOSSurfaceCreateFlagsINSTANTOS;
typedef struct VkInstantOSSurfaceCreateInfoINSTANTOS {
    VkStructureType                           sType;
    const void*                               pNext;
    VkInstantOSSurfaceCreateFlagsINSTANTOS    flags;
    uint64_t                                  window;
} VkInstantOSSurfaceCreateInfoINSTANTOS;

typedef VkResult (VKAPI_PTR *PFN_vkCreateInstantOSSurfaceINSTANTOS)(VkInstance instance, const VkInstantOSSurfaceCreateInfoINSTANTOS* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);

#ifndef VK_NO_PROTOTYPES
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstantOSSurfaceINSTANTOS(
    VkInstance                                  instance,
    const VkInstantOSSurfaceCreateInfoINSTANTOS* pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSurfaceKHR*                               pSurface);
#endif

#ifdef __cplusplus
}
#endif

#endif
