/*
 * EGL on InstantOS: window surfaces in compositor windows.
 *
 * The native display is EGL_DEFAULT_DISPLAY; the native window is a
 * struct instantos_egl_window (<instantos/window.h>): the compositor window
 * and the size its frames should have. The GPU is the render node, found as
 * the surfaceless platform finds it.
 *
 * Each window surface draws into up to three linear back buffers: dri images
 * exported as dma-bufs, each made a kernel surface (SurfaceFromBuffer).
 * eglSwapBuffers flushes, attaches the back buffer to the window and commits
 * it (the kernel shows it once the GPU has finished drawing it), and the next
 * frame takes a buffer the compositor has let go (SurfaceWaitIdle): the
 * compositor flips once a frame, so this paces the program to the display.
 * Pbuffers are as on the surfaceless platform.
 *
 * Based on platform_surfaceless.c (MIT, see its notice), for InstantOS.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util/libdrm.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <instantos/window.h>

#include "util/format/u_formats.h"
#include "main/glconfig.h"
#include "egl_dri2.h"
#include "eglglobals.h"
#include "kopper_interface.h"
#include "loader.h"
#include "loader_dri_helper.h"
#include "dri_util.h"

#define INSTANTOS_BUFFERS 3

static struct dri_image *
instantos_alloc_image(struct dri2_egl_display *dri2_dpy, struct dri2_egl_surface *dri2_surf, unsigned use)
{
   return dri_create_image(dri2_dpy->dri_screen_render_gpu, dri2_surf->base.Width, dri2_surf->base.Height,
                           dri2_surf->visual, NULL, 0, use, NULL);
}

static void
instantos_free_buffer(struct dri2_egl_surface *dri2_surf, int i)
{
   if (dri2_surf->instantos_buffers[i].surface > 0)
      instantos_handle_close(dri2_surf->instantos_buffers[i].surface);
   if (dri2_surf->instantos_buffers[i].fd >= 0)
      close(dri2_surf->instantos_buffers[i].fd);
   if (dri2_surf->instantos_buffers[i].dri_image)
      dri2_destroy_image(dri2_surf->instantos_buffers[i].dri_image);
   memset(&dri2_surf->instantos_buffers[i], 0, sizeof(dri2_surf->instantos_buffers[i]));
   dri2_surf->instantos_buffers[i].fd = -1;
}

static void
instantos_free_images(struct dri2_egl_surface *dri2_surf)
{
   if (dri2_surf->front) {
      dri2_destroy_image(dri2_surf->front);
      dri2_surf->front = NULL;
   }
   for (int i = 0; i < INSTANTOS_BUFFERS; i++)
      instantos_free_buffer(dri2_surf, i);
   dri2_surf->instantos_back = NULL;
   dri2_surf->instantos_current = NULL;
   free(dri2_surf->swrast_device_buffer);
   dri2_surf->swrast_device_buffer = NULL;
}

/* A linear image the compositor can map, as a surface. */
static bool
instantos_make_buffer(struct dri2_egl_display *dri2_dpy, struct dri2_egl_surface *dri2_surf, int i)
{
   struct dri_image *image =
      instantos_alloc_image(dri2_dpy, dri2_surf, __DRI_IMAGE_USE_LINEAR | __DRI_IMAGE_USE_SHARE);
   if (!image)
      return false;
   int fd = -1, stride = 0;
   if (!dri2_query_image(image, __DRI_IMAGE_ATTRIB_FD, &fd) ||
       !dri2_query_image(image, __DRI_IMAGE_ATTRIB_STRIDE, &stride) || fd < 0) {
      _eglLog(_EGL_WARNING, "instantos: cannot export a back buffer");
      if (fd >= 0)
         close(fd);
      dri2_destroy_image(image);
      return false;
   }
   long surface = instantos_surface_from_buffer(fd, dri2_surf->base.Width, dri2_surf->base.Height, stride);
   if (surface < 0) {
      _eglLog(_EGL_WARNING, "instantos: SurfaceFromBuffer failed (%ld)", surface);
      close(fd);
      dri2_destroy_image(image);
      return false;
   }
   dri2_surf->instantos_buffers[i].dri_image = image;
   dri2_surf->instantos_buffers[i].fd = fd;
   dri2_surf->instantos_buffers[i].surface = surface;
   dri2_surf->instantos_buffers[i].width = dri2_surf->base.Width;
   dri2_surf->instantos_buffers[i].height = dri2_surf->base.Height;
   dri2_surf->instantos_buffers[i].age = 0;
   return true;
}

/* Follows the native window's size: the buffers are made again at the new one. */
static void
instantos_update_size(struct dri2_egl_surface *dri2_surf)
{
   struct instantos_egl_window *win = dri2_surf->instantos_win;
   if (!win || win->width <= 0 || win->height <= 0)
      return;
   if (win->width == dri2_surf->base.Width && win->height == dri2_surf->base.Height)
      return;
   dri2_surf->base.Width = win->width;
   dri2_surf->base.Height = win->height;
   for (int i = 0; i < INSTANTOS_BUFFERS; i++) {
      if (&dri2_surf->instantos_buffers[i] != dri2_surf->instantos_current)
         instantos_free_buffer(dri2_surf, i);
   }
   dri2_surf->instantos_back = NULL;
}

/* A buffer to draw the next frame into: a free one (never shown, or let go
 * by the compositor), made if need be; else the oldest, once it is free. */
static bool
instantos_get_back(struct dri2_egl_display *dri2_dpy, struct dri2_egl_surface *dri2_surf)
{
   if (dri2_surf->instantos_back)
      return true;
   instantos_update_size(dri2_surf);
   int empty = -1, oldest = -1;
   for (int i = 0; i < INSTANTOS_BUFFERS; i++) {
      if (&dri2_surf->instantos_buffers[i] == dri2_surf->instantos_current)
         continue;
      if (!dri2_surf->instantos_buffers[i].dri_image) {
         if (empty < 0)
            empty = i;
         continue;
      }
      if (instantos_surface_wait_idle(dri2_surf->instantos_buffers[i].surface, 0) == 0) {
         dri2_surf->instantos_back = &dri2_surf->instantos_buffers[i];
         return true;
      }
      if (oldest < 0 || dri2_surf->instantos_buffers[i].age > dri2_surf->instantos_buffers[oldest].age)
         oldest = i;
   }
   if (empty >= 0 && instantos_make_buffer(dri2_dpy, dri2_surf, empty)) {
      dri2_surf->instantos_back = &dri2_surf->instantos_buffers[empty];
      return true;
   }
   if (oldest < 0)
      return false;
   /* Every buffer is still on its way out of the compositor: wait (it lets
    * one go at its next frame). */
   if (instantos_surface_wait_idle(dri2_surf->instantos_buffers[oldest].surface, 1000) != 0)
      _eglLog(_EGL_WARNING, "instantos: a back buffer stayed busy for 1 s");
   dri2_surf->instantos_back = &dri2_surf->instantos_buffers[oldest];
   return true;
}

static int
instantos_image_get_buffers(struct dri_drawable *driDrawable, unsigned int format, uint32_t *stamp,
                            void *loaderPrivate, uint32_t buffer_mask, struct __DRIimageList *buffers)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dri2_surf->base.Resource.Display);

   buffers->image_mask = 0;
   buffers->front = NULL;
   buffers->back = NULL;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      if (buffer_mask & __DRI_IMAGE_BUFFER_BACK) {
         if (!instantos_get_back(dri2_dpy, dri2_surf))
            return 0;
         buffers->image_mask |= __DRI_IMAGE_BUFFER_BACK;
         buffers->back = dri2_surf->instantos_back->dri_image;
      }
      return 1;
   }

   /* Pbuffers: a front buffer only, as the surfaceless platform does. */
   if (buffer_mask & __DRI_IMAGE_BUFFER_FRONT) {
      if (!dri2_surf->front) {
         dri2_surf->front = instantos_alloc_image(dri2_dpy, dri2_surf, 0);
         if (!dri2_surf->front)
            return 0;
      }
      buffers->image_mask |= __DRI_IMAGE_BUFFER_FRONT;
      buffers->front = dri2_surf->front;
   }
   return 1;
}

static _EGLSurface *
dri2_instantos_create_surface(_EGLDisplay *disp, EGLint type, _EGLConfig *conf, void *native_window,
                              const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   const struct dri_config *config;

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "eglCreateSurface");
      return NULL;
   }
   for (int i = 0; i < INSTANTOS_BUFFERS; i++)
      dri2_surf->instantos_buffers[i].fd = -1;

   if (!dri2_init_surface(&dri2_surf->base, disp, type, conf, attrib_list, false, native_window))
      goto cleanup_surface;

   if (type == EGL_WINDOW_BIT) {
      struct instantos_egl_window *win = native_window;
      dri2_surf->instantos_win = win;
      dri2_surf->base.Width = win->width;
      dri2_surf->base.Height = win->height;
   }

   config = dri2_get_dri_config(dri2_conf, type, dri2_surf->base.GLColorspace);
   if (!config) {
      _eglError(EGL_BAD_MATCH, "Unsupported surfacetype/colorspace configuration");
      goto cleanup_surface;
   }

   dri2_surf->visual = dri2_image_format_for_pbuffer_config(dri2_dpy, config);
   if (dri2_surf->visual == PIPE_FORMAT_NONE)
      goto cleanup_surface;

   if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
      goto cleanup_surface;

   return &dri2_surf->base;

cleanup_surface:
   free(dri2_surf);
   return NULL;
}

static _EGLSurface *
dri2_instantos_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf, void *native_window,
                                     const EGLint *attrib_list)
{
   struct instantos_egl_window *win = native_window;
   if (!win || win->width <= 0 || win->height <= 0) {
      _eglError(EGL_BAD_NATIVE_WINDOW, "eglCreateWindowSurface");
      return NULL;
   }
   return dri2_instantos_create_surface(disp, EGL_WINDOW_BIT, conf, native_window, attrib_list);
}

static _EGLSurface *
dri2_instantos_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf, const EGLint *attrib_list)
{
   return dri2_instantos_create_surface(disp, EGL_PBUFFER_BIT, conf, NULL, attrib_list);
}

static EGLBoolean
instantos_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   instantos_free_images(dri2_surf);
   driDestroyDrawable(dri2_surf->dri_drawable);
   dri2_fini_surface(surf);
   free(dri2_surf);
   return EGL_TRUE;
}

static EGLBoolean
dri2_instantos_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   if (draw->Type != EGL_WINDOW_BIT)
      return EGL_TRUE;
   if (!dri2_surf->instantos_win)
      return _eglError(EGL_BAD_NATIVE_WINDOW, "eglSwapBuffers");

   dri2_flush_drawable_for_swapbuffers(disp, draw);
   dri_invalidate_drawable(dri2_surf->dri_drawable);

   /* A swap without drawing still shows a frame. */
   if (!instantos_get_back(dri2_dpy, dri2_surf))
      return _eglError(EGL_BAD_ALLOC, "eglSwapBuffers");

   for (int i = 0; i < INSTANTOS_BUFFERS; i++)
      if (dri2_surf->instantos_buffers[i].age > 0)
         dri2_surf->instantos_buffers[i].age++;

   dri2_surf->instantos_current = dri2_surf->instantos_back;
   dri2_surf->instantos_back = NULL;
   dri2_surf->instantos_current->age = 1;
   instantos_window_present(dri2_surf->instantos_win->window, dri2_surf->instantos_current->surface,
                            dri2_surf->instantos_current->width, dri2_surf->instantos_current->height);
   return EGL_TRUE;
}

static EGLint
dri2_instantos_query_buffer_age(_EGLDisplay *disp, _EGLSurface *surface)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surface);

   if (!instantos_get_back(dri2_dpy, dri2_surf))
      return -1;
   return dri2_surf->instantos_back->age;
}

static const struct dri2_egl_display_vtbl dri2_instantos_display_vtbl = {
   .create_window_surface = dri2_instantos_create_window_surface,
   .create_pbuffer_surface = dri2_instantos_create_pbuffer_surface,
   .destroy_surface = instantos_destroy_surface,
   .create_image = dri2_create_image_khr,
   .swap_buffers = dri2_instantos_swap_buffers,
   .query_buffer_age = dri2_instantos_query_buffer_age,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static void
instantos_flush_front_buffer(struct dri_drawable *driDrawable, void *loaderPrivate)
{
}

static unsigned
instantos_get_capability(void *loaderPrivate, enum dri_loader_cap cap)
{
   switch (cap) {
   case DRI_LOADER_CAP_FP16:
      return 1;
   case DRI_LOADER_CAP_RGBA_ORDERING:
      return 1;
   default:
      return 0;
   }
}

static const __DRIkopperLoaderExtension kopper_loader_extension = {
   .base = {__DRI_KOPPER_LOADER, 1},
   .SetSurfaceCreateInfo = NULL,
};

static const __DRIimageLoaderExtension image_loader_extension = {
   .base = {__DRI_IMAGE_LOADER, 2},
   .getBuffers = instantos_image_get_buffers,
   .flushFrontBuffer = instantos_flush_front_buffer,
   .getCapability = instantos_get_capability,
};

static const __DRIextension *image_loader_extensions[] = {
   &image_loader_extension.base, &image_lookup_extension.base,  &use_invalidate.base,
   &background_callable_extension.base, &kopper_loader_extension.base, NULL,
};

static bool
instantos_probe_device(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   _EGLDevice *dev_list = _eglGlobal.DeviceList;
   drmDevicePtr device;

   while (dev_list) {
      if (!_eglDeviceSupports(dev_list, _EGL_DEVICE_DRM))
         goto next;

      device = _eglDeviceDrm(dev_list);
      assert(device);

      if (!(device->available_nodes & (1 << DRM_NODE_RENDER)))
         goto next;

      /* The kernel's render node without a GPU (gpu-test's QEMU checks)
       * reports zero ids: no driver can run on it. */
      if (device->bustype == DRM_BUS_PCI && device->deviceinfo.pci && device->deviceinfo.pci->vendor_id == 0)
         goto next;

      dri2_dpy->fd_render_gpu = loader_open_device(device->nodes[DRM_NODE_RENDER]);
      if (dri2_dpy->fd_render_gpu < 0)
         goto next;

      disp->Device = dev_list;
      dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd_render_gpu);

      if (dri2_dpy->driver_name && dri2_load_driver(disp)) {
         dri2_dpy->loader_extensions = image_loader_extensions;
         break;
      }

      free(dri2_dpy->driver_name);
      dri2_dpy->driver_name = NULL;
      close(dri2_dpy->fd_render_gpu);
      dri2_dpy->fd_render_gpu = -1;

   next:
      dev_list = _eglDeviceNext(dev_list);
   }

   return dev_list != NULL;
}

/* Window and pbuffer configs in the compositor's pixel format (BGRA in
 * memory), pbuffer-only ones for the rest. */
static void
instantos_add_configs(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   unsigned windows = 0;

   for (unsigned i = 0; dri2_dpy->driver_configs[i] != NULL; i++) {
      struct gl_config *gl_config = (struct gl_config *)dri2_dpy->driver_configs[i];
      const bool window = gl_config->color_format == PIPE_FORMAT_B8G8R8A8_UNORM ||
                          gl_config->color_format == PIPE_FORMAT_B8G8R8X8_UNORM;
      if (window) {
         if (dri2_add_config(disp, dri2_dpy->driver_configs[i], EGL_WINDOW_BIT | EGL_PBUFFER_BIT, NULL))
            windows++;
      }
   }
   if (windows == 0)
      _eglLog(_EGL_WARNING, "instantos: no config for window surfaces");
   dri2_add_pbuffer_configs_for_visuals(disp);
}

EGLBoolean
dri2_initialize_instantos(_EGLDisplay *disp)
{
   const char *err;
   struct dri2_egl_display *dri2_dpy = dri2_display_create();
   if (!dri2_dpy)
      return EGL_FALSE;

   disp->DriverData = (void *)dri2_dpy;

   if (!instantos_probe_device(disp)) {
      err = "DRI2: no GPU driver for the render node";
      goto cleanup;
   }

   dri2_dpy->fd_display_gpu = dri2_dpy->fd_render_gpu;

   if (!dri2_create_screen(disp)) {
      err = "DRI2: failed to create screen";
      goto cleanup;
   }

   dri2_setup_screen(disp);
   instantos_add_configs(disp);

   disp->Extensions.EXT_buffer_age = EGL_TRUE;

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = &dri2_instantos_display_vtbl;

   return EGL_TRUE;

cleanup:
   dri2_display_destroy(disp);
   return _eglError(EGL_NOT_INITIALIZED, err);
}
