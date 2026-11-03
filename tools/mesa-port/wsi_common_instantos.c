/*
 * Vulkan WSI on InstantOS (VK_INSTANTOS_surface): swapchains in compositor
 * windows.
 *
 * A surface is a compositor window (VkIcdSurfaceInstantOS). Its swapchain's
 * images are linear dma-bufs (an explicit DRM_FORMAT_MOD_LINEAR when the
 * driver takes modifiers, else Mesa's blit into a linear buffer), each made a
 * kernel surface (SurfaceFromBuffer), as the EGL platform's back buffers are
 * (platform_instantos.c). Rendering into an image marks its buffer written
 * (implicit sync, as on X11 and Wayland); vkQueuePresentKHR commits the image
 * (the kernel shows it once the GPU has finished drawing it) and attaches it
 * to the window. An image is free to acquire again once the compositor has
 * let it go (SurfaceWaitIdle); the compositor flips once a frame, so FIFO is
 * paced by the display.
 *
 * Based on wsi_common_headless.c (Copyright 2021 Red Hat, Inc., MIT, notice
 * below), for InstantOS.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <instantos/window.h>

#include "util/macros.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"

#include "drm-uapi/drm_fourcc.h"

/* At least: one the compositor shows, one on its way out of it (it lets a
 * surface go at its next frame), one to draw into. */
#define INSTANTOS_MIN_IMAGES 3
#define INSTANTOS_MAX_IMAGES 8

struct wsi_instantos {
   struct wsi_interface base;
   struct wsi_device *wsi;
   const VkAllocationCallbacks *alloc;
   VkPhysicalDevice physical_device;
};

static VkResult
wsi_instantos_surface_get_support(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                                  uint32_t queueFamilyIndex, VkBool32 *pSupported)
{
   *pSupported = true;
   return VK_SUCCESS;
}

static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_FIFO_KHR,
};

static VkResult
wsi_instantos_surface_get_capabilities(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                                       VkSurfaceCapabilitiesKHR *caps)
{
   caps->minImageCount = INSTANTOS_MIN_IMAGES;
   caps->maxImageCount = INSTANTOS_MAX_IMAGES;
   /* The swapchain's extent is the window's size (as on Wayland). */
   caps->currentExtent = (VkExtent2D){UINT32_MAX, UINT32_MAX};
   caps->minImageExtent = (VkExtent2D){1, 1};
   caps->maxImageExtent = (VkExtent2D){wsi_device->maxImageDimension2D, wsi_device->maxImageDimension2D};
   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->maxImageArrayLayers = 1;
   /* The compositor draws a window's surface opaque. */
   caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   caps->supportedUsageFlags = wsi_caps_get_image_usage();

   VK_FROM_HANDLE(vk_physical_device, pdevice, wsi_device->pdevice);
   if (pdevice->supported_extensions.EXT_attachment_feedback_loop_layout)
      caps->supportedUsageFlags |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
   return VK_SUCCESS;
}

static VkResult
wsi_instantos_surface_get_capabilities2(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                                        const void *info_next, VkSurfaceCapabilities2KHR *caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   VkResult result = wsi_instantos_surface_get_capabilities(surface, wsi_device, &caps->surfaceCapabilities);

   vk_foreach_struct(ext, caps->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected = (void *)ext;
         protected->supportsProtected = VK_FALSE;
         break;
      }
      default:
         break;
      }
   }
   return result;
}

/* The compositor's pixel format is BGRA in memory. */
static const VkFormat surface_formats[] = {
   VK_FORMAT_B8G8R8A8_SRGB,
   VK_FORMAT_B8G8R8A8_UNORM,
};

static VkResult
wsi_instantos_surface_get_formats(VkIcdSurfaceBase *icd_surface, struct wsi_device *wsi_device,
                                  uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (unsigned i = 0; i < ARRAY_SIZE(surface_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, out_fmt) {
         out_fmt->format = surface_formats[i];
         out_fmt->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
wsi_instantos_surface_get_formats2(VkIcdSurfaceBase *icd_surface, struct wsi_device *wsi_device,
                                   const void *info_next, uint32_t *pSurfaceFormatCount,
                                   VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (unsigned i = 0; i < ARRAY_SIZE(surface_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, out_fmt) {
         out_fmt->surfaceFormat.format = surface_formats[i];
         out_fmt->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
wsi_instantos_surface_get_present_modes(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                                        uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }
   *pPresentModeCount = MIN2(*pPresentModeCount, ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pPresentModeCount);
   return *pPresentModeCount < ARRAY_SIZE(present_modes) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult
wsi_instantos_surface_get_present_rectangles(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                                             uint32_t *pRectCount, VkRect2D *pRects)
{
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);
   vk_outarray_append_typed(VkRect2D, &out, rect) {
      *rect = (VkRect2D){.offset = {0, 0}, .extent = {UINT32_MAX, UINT32_MAX}};
   }
   return vk_outarray_status(&out);
}

struct wsi_instantos_image {
   struct wsi_image base;
   long surface;        /* the kernel surface on its dma-buf */
   bool acquired;       /* the application holds it */
   uint64_t presented;  /* when it was last presented (0: never) */
};

struct wsi_instantos_swapchain {
   struct wsi_swapchain base;
   uint64_t window;
   VkExtent2D extent;
   uint64_t presents;
   struct wsi_instantos_image images[0];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wsi_instantos_swapchain, base.base, VkSwapchainKHR, VK_OBJECT_TYPE_SWAPCHAIN_KHR)

static struct wsi_image *
wsi_instantos_swapchain_get_wsi_image(struct wsi_swapchain *wsi_chain, uint32_t image_index)
{
   struct wsi_instantos_swapchain *chain = (struct wsi_instantos_swapchain *)wsi_chain;
   return &chain->images[image_index].base;
}

static VkResult
wsi_instantos_swapchain_acquire_next_image(struct wsi_swapchain *wsi_chain, const VkAcquireNextImageInfoKHR *info,
                                           uint32_t *image_index)
{
   struct wsi_instantos_swapchain *chain = (struct wsi_instantos_swapchain *)wsi_chain;

   /* A free image: never shown, or let go by the compositor. Else wait for
    * the oldest one shown: the compositor lets it go at its next frame (the
    * newest stays on screen until the next present, so with the images the
    * application may hold, an older one is always there). */
   int oldest = -1;
   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      struct wsi_instantos_image *image = &chain->images[i];
      if (image->acquired)
         continue;
      if (image->presented == 0 || instantos_surface_wait_idle(image->surface, 0) == 0) {
         image->acquired = true;
         *image_index = i;
         return VK_SUCCESS;
      }
      if (oldest < 0 || image->presented < chain->images[oldest].presented)
         oldest = i;
   }
   if (info->timeout == 0)
      return VK_NOT_READY;
   if (oldest < 0)
      return VK_TIMEOUT;   /* the application holds every image */
   const uint64_t timeout_ms = info->timeout == UINT64_MAX ? 5000 : (info->timeout + 999999) / 1000000;
   if (instantos_surface_wait_idle(chain->images[oldest].surface, timeout_ms) != 0)
      return VK_TIMEOUT;
   chain->images[oldest].acquired = true;
   *image_index = oldest;
   return VK_SUCCESS;
}

static VkResult
wsi_instantos_swapchain_queue_present(struct wsi_swapchain *wsi_chain, uint32_t image_index, uint64_t present_id,
                                      const VkPresentRegionKHR *damage)
{
   struct wsi_instantos_swapchain *chain = (struct wsi_instantos_swapchain *)wsi_chain;
   assert(image_index < chain->base.image_count);
   struct wsi_instantos_image *image = &chain->images[image_index];

   /* The rendering was submitted with the image's buffer marked written:
    * the commit waits for it, so the window never shows a frame half drawn. */
   instantos_window_present(chain->window, image->surface, chain->extent.width, chain->extent.height);
   image->acquired = false;
   image->presented = ++chain->presents;
   return VK_SUCCESS;
}

static VkResult
wsi_instantos_swapchain_destroy(struct wsi_swapchain *wsi_chain, const VkAllocationCallbacks *pAllocator)
{
   struct wsi_instantos_swapchain *chain = (struct wsi_instantos_swapchain *)wsi_chain;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].surface > 0)
         instantos_handle_close(chain->images[i].surface);
      if (chain->images[i].base.image != VK_NULL_HANDLE)
         wsi_destroy_image(&chain->base, &chain->images[i].base);
   }
   wsi_swapchain_finish(&chain->base);
   vk_free(pAllocator, chain);
   return VK_SUCCESS;
}

static VkResult
wsi_instantos_surface_create_swapchain(VkIcdSurfaceBase *icd_surface, VkDevice device,
                                       struct wsi_device *wsi_device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                       const VkAllocationCallbacks *pAllocator, struct wsi_swapchain **swapchain_out)
{
   VkIcdSurfaceInstantOS *surface = (VkIcdSurfaceInstantOS *)icd_surface;
   struct wsi_instantos_swapchain *chain;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   const uint32_t num_images = MAX2(pCreateInfo->minImageCount, INSTANTOS_MIN_IMAGES);
   const size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);
   chain = vk_zalloc(pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* Linear, so the compositor can read it. */
   static const uint64_t linear = DRM_FORMAT_MOD_LINEAR;
   const uint64_t *modifier_lists[1] = {&linear};
   uint32_t modifier_counts[1] = {1};
   struct wsi_drm_image_params drm_params = {
      .base.image_type = WSI_IMAGE_TYPE_DRM,
      .same_gpu = true,
      .explicit_sync = false,
   };
   if (wsi_device->supports_modifiers) {
      drm_params.num_modifier_lists = 1;
      drm_params.num_modifiers = modifier_counts;
      drm_params.modifiers = modifier_lists;
   }

   result = wsi_swapchain_init(wsi_device, &chain->base, device, pCreateInfo, &drm_params.base, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, chain);
      return result;
   }

   chain->base.destroy = wsi_instantos_swapchain_destroy;
   chain->base.get_wsi_image = wsi_instantos_swapchain_get_wsi_image;
   chain->base.acquire_next_image = wsi_instantos_swapchain_acquire_next_image;
   chain->base.queue_present = wsi_instantos_swapchain_queue_present;
   chain->base.present_mode = wsi_swapchain_get_present_mode(wsi_device, pCreateInfo);
   chain->base.image_count = num_images;
   chain->window = surface->window;
   chain->extent = pCreateInfo->imageExtent;

   result = wsi_drm_configure_image(&chain->base, pCreateInfo, &drm_params, &chain->base.image_info);
   if (result != VK_SUCCESS)
      goto fail;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      struct wsi_instantos_image *image = &chain->images[i];
      result = wsi_create_image(&chain->base, &chain->base.image_info, &image->base);
      if (result != VK_SUCCESS)
         goto fail;
      if (image->base.dma_buf_fd < 0 || image->base.num_planes != 1 || image->base.offsets[0] != 0) {
         result = VK_ERROR_INITIALIZATION_FAILED;
         goto fail;
      }
      image->surface = instantos_surface_from_buffer(image->base.dma_buf_fd, chain->extent.width,
                                                     chain->extent.height, image->base.row_pitches[0]);
      if (image->surface < 0) {
         image->surface = 0;
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         goto fail;
      }
   }

   *swapchain_out = &chain->base;
   return VK_SUCCESS;

fail:
   wsi_instantos_swapchain_destroy(&chain->base, pAllocator);
   return result;
}

VkResult
wsi_instantos_init_wsi(struct wsi_device *wsi_device, const VkAllocationCallbacks *alloc,
                       VkPhysicalDevice physical_device)
{
   struct wsi_instantos *wsi = vk_alloc(alloc, sizeof(*wsi), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi) {
      wsi_device->wsi[VK_ICD_WSI_PLATFORM_INSTANTOS] = NULL;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   wsi->physical_device = physical_device;
   wsi->alloc = alloc;
   wsi->wsi = wsi_device;

   wsi->base.get_support = wsi_instantos_surface_get_support;
   wsi->base.get_capabilities2 = wsi_instantos_surface_get_capabilities2;
   wsi->base.get_formats = wsi_instantos_surface_get_formats;
   wsi->base.get_formats2 = wsi_instantos_surface_get_formats2;
   wsi->base.get_present_modes = wsi_instantos_surface_get_present_modes;
   wsi->base.get_present_rectangles = wsi_instantos_surface_get_present_rectangles;
   wsi->base.create_swapchain = wsi_instantos_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_INSTANTOS] = &wsi->base;
   return VK_SUCCESS;
}

void
wsi_instantos_finish_wsi(struct wsi_device *wsi_device, const VkAllocationCallbacks *alloc)
{
   struct wsi_instantos *wsi = (struct wsi_instantos *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_INSTANTOS];
   if (!wsi)
      return;
   vk_free(alloc, wsi);
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateInstantOSSurfaceINSTANTOS(VkInstance _instance, const VkInstantOSSurfaceCreateInfoINSTANTOS *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANTOS_SURFACE_CREATE_INFO_INSTANTOS);

   VkIcdSurfaceInstantOS *surface =
      vk_alloc2(&instance->alloc, pAllocator, sizeof *surface, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_INSTANTOS;
   surface->window = pCreateInfo->window;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}
