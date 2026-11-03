#!/usr/bin/env python3
"""Source fixes for Mesa 24.3.4 as InstantOS builds it (tools/build-mesa.sh).
Each is idempotent (skipped when already applied), named, and small.

Usage: patch-mesa.py MESA_SOURCE_DIR
"""
import sys
from pathlib import Path

ROOT = Path(sys.argv[1])
applied = []


def fix(name, rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if text.count(old) != 1:
        raise SystemExit(f"{name}: expected one match in {rel}")
    path.write_text(text.replace(old, new), encoding="utf-8")
    applied.append(name)


# LLVM 20 takes the file system first in CompilerInstance::createDiagnostics
# (Mesa 25.0's "clc: fix build with LLVM 20"); the host intel_clc build uses
# Ubuntu's LLVM 20.
fix("clc-llvm20", "src/compiler/clc/clc_helpers.cpp",
    """   c->createDiagnostics(new clang::TextDiagnosticPrinter(
                           diag_log_stream,
                           &c->getDiagnosticOpts()));""",
    """#if LLVM_VERSION_MAJOR >= 20
   c->createDiagnostics(*llvm::vfs::getRealFileSystem(),
                        new clang::TextDiagnosticPrinter(
                           diag_log_stream,
                           &c->getDiagnosticOpts()));
#else
   c->createDiagnostics(new clang::TextDiagnosticPrinter(
                           diag_log_stream,
                           &c->getDiagnosticOpts()));
#endif""")

# Futexes on InstantOS (__instantos__): the kernel has none yet, so a wait
# yields until the word changes or the absolute CLOCK_MONOTONIC deadline
# passes, and a wake does nothing (mlibc's own futex sysdeps do the same).
# anv's block pool calls futex_wait/futex_wake directly.
fix("futex-supported", "src/util/futex.h",
    """#elif defined(__FreeBSD__)
#define UTIL_FUTEX_SUPPORTED 1""",
    """#elif defined(__instantos__)
#define UTIL_FUTEX_SUPPORTED 1
#elif defined(__FreeBSD__)
#define UTIL_FUTEX_SUPPORTED 1""")
fix("futex-instantos", "src/util/futex.c",
    """#if UTIL_FUTEX_SUPPORTED

#if defined(HAVE_LINUX_FUTEX_H)
""",
    """#if UTIL_FUTEX_SUPPORTED

#if defined(__instantos__)

#include <sched.h>
#include <time.h>

int futex_wake(uint32_t *addr, int32_t count)
{
   (void)addr;
   (void)count;
   return 0;   /* the waiters poll */
}

int futex_wait(uint32_t *addr, int32_t value, const struct timespec *timeout)
{
   while (__atomic_load_n(addr, __ATOMIC_ACQUIRE) == (uint32_t)value) {
      if (timeout) {
         struct timespec now;
         clock_gettime(CLOCK_MONOTONIC, &now);
         if (now.tv_sec > timeout->tv_sec ||
             (now.tv_sec == timeout->tv_sec && now.tv_nsec >= timeout->tv_nsec))
            return ETIMEDOUT;
      }
      sched_yield();
   }
   return 0;
}

#elif defined(HAVE_LINUX_FUTEX_H)
""")
fix("futex-errno", "src/util/futex.c",
    """#include "util/futex.h"
""",
    """#include "util/futex.h"
#include <errno.h>
""")

# No sched_getcpu in mlibc without its linux option.
fix("getcpu", "src/util/u_thread.c",
    """#if DETECT_OS_LINUX && !DETECT_OS_ANDROID
   return sched_getcpu();""",
    """#if DETECT_OS_LINUX && !DETECT_OS_ANDROID && !defined(__instantos__)
   return sched_getcpu();""")

# No raw syscall() (gettid) for the low-priority queue threads.
fix("queue-syscall-include", "src/util/u_queue.c",
    """#if defined(__linux__)
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#endif""",
    """#if defined(__linux__) && !defined(__instantos__)
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#endif""")
fix("queue-priority", "src/util/u_queue.c",
    """#if defined(__linux__)
   if (queue->flags & UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY) {""",
    """#if defined(__linux__) && !defined(__instantos__)
   if (queue->flags & UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY) {""")

# anv's state table is a 2 GiB anonymous file (memfd_create, or a file in
# XDG_RUNTIME_DIR) that it maps again, larger, whenever it grows, the old maps
# still aliasing the same pages. InstantOS has neither memfd nor a runtime
# dir, so device creation failed there (a bare VK_ERROR_INITIALIZATION_FAILED).
# Instead the whole 2 GiB is reserved once as demand-paged anonymous memory:
# the base never moves, so growing only raises the size and nothing aliases.
fix("anv-state-table-init", "src/intel/vulkan/anv_allocator.c",
    """   table->fd = os_create_anonymous_file(BLOCK_POOL_MEMFD_SIZE, "state table");
   if (table->fd == -1)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
""",
    """#ifdef __instantos__
   table->fd = -1;
   table->map = mmap(NULL, BLOCK_POOL_MEMFD_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (table->map == MAP_FAILED)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "state table reservation failed: %m");
#else
   table->fd = os_create_anonymous_file(BLOCK_POOL_MEMFD_SIZE, "state table");
   if (table->fd == -1)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
#endif
""")
fix("anv-state-table-expand", "src/intel/vulkan/anv_allocator.c",
    """   cleanup = u_vector_add(&table->cleanups);
   if (!cleanup)
      return vk_error(table->device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *cleanup = ANV_STATE_TABLE_CLEANUP_INIT;
""",
    """#ifdef __instantos__
   /* The reservation made at init is the map; the first growth records it
    * for anv_state_table_finish to unmap. */
   if (table->size == 0) {
      cleanup = u_vector_add(&table->cleanups);
      if (!cleanup)
         return vk_error(table->device, VK_ERROR_OUT_OF_HOST_MEMORY);
      cleanup->map = table->map;
      cleanup->size = BLOCK_POOL_MEMFD_SIZE;
   }
   table->size = size;
   return VK_SUCCESS;
#endif

   cleanup = u_vector_add(&table->cleanups);
   if (!cleanup)
      return vk_error(table->device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *cleanup = ANV_STATE_TABLE_CLEANUP_INIT;
""")

# The InstantOS EGL platform (tools/mesa-port/platform_instantos.c): window
# surfaces in compositor windows, EGL_DEFAULT_DISPLAY's platform
# (-Dplatforms=instantos -Degl-native-platform=instantos).
platform_source = Path(__file__).resolve().parent / "platform_instantos.c"
platform_target = ROOT / "src/egl/drivers/dri2/platform_instantos.c"
if not platform_target.exists() or platform_target.read_text(encoding="utf-8") != platform_source.read_text(encoding="utf-8"):
    platform_target.write_text(platform_source.read_text(encoding="utf-8"), encoding="utf-8")
    applied.append("egl-platform-source")
fix("egl-platform-option", "meson_options.txt",
    """    'auto', 'x11', 'wayland', 'haiku', 'android', 'windows', 'macos',
  ],""",
    """    'auto', 'x11', 'wayland', 'haiku', 'android', 'windows', 'macos',
    'instantos',
  ],""")
fix("egl-platform-native-option", "meson_options.txt",
    """    'auto', 'x11', 'wayland', 'haiku', 'android', 'windows',
    'surfaceless', 'drm',
  ],""",
    """    'auto', 'x11', 'wayland', 'haiku', 'android', 'windows',
    'surfaceless', 'drm', 'instantos',
  ],""")
fix("egl-platform-meson", "meson.build",
    """with_platform_macos = _platforms.contains('macos')
""",
    """with_platform_macos = _platforms.contains('macos')
with_platform_instantos = _platforms.contains('instantos')
""")
fix("egl-platform-files", "src/egl/meson.build",
    """    'drivers/dri2/platform_surfaceless.c',
  )
""",
    """    'drivers/dri2/platform_surfaceless.c',
  )
  if with_platform_instantos
    files_egl += files('drivers/dri2/platform_instantos.c')
  endif
""")
fix("egl-platform-enum", "src/egl/main/egldisplay.h",
    """   _EGL_PLATFORM_WINDOWS,

   _EGL_NUM_PLATFORMS,""",
    """   _EGL_PLATFORM_WINDOWS,
   _EGL_PLATFORM_INSTANTOS,

   _EGL_NUM_PLATFORMS,""")
fix("egl-platform-name", "src/egl/main/egldisplay.c",
    """   {_EGL_PLATFORM_WINDOWS, "windows"},
};""",
    """   {_EGL_PLATFORM_WINDOWS, "windows"},
   {_EGL_PLATFORM_INSTANTOS, "instantos"},
};""")
fix("egl-platform-init", "src/egl/drivers/dri2/egl_dri2.c",
    """   case _EGL_PLATFORM_ANDROID:
      ret = dri2_initialize_android(disp);
      break;
   default:""",
    """   case _EGL_PLATFORM_ANDROID:
      ret = dri2_initialize_android(disp);
      break;
   case _EGL_PLATFORM_INSTANTOS:
      ret = dri2_initialize_instantos(disp);
      break;
   default:""")
fix("egl-platform-decl", "src/egl/drivers/dri2/egl_dri2.h",
    """EGLBoolean
dri2_initialize_surfaceless(_EGLDisplay *disp);
""",
    """EGLBoolean
dri2_initialize_surfaceless(_EGLDisplay *disp);

#ifdef HAVE_INSTANTOS_PLATFORM
EGLBoolean
dri2_initialize_instantos(_EGLDisplay *disp);
#else
static inline EGLBoolean
dri2_initialize_instantos(_EGLDisplay *disp)
{
   return _eglError(EGL_NOT_INITIALIZED, "InstantOS platform not built");
}
#endif
""")
fix("egl-platform-surface", "src/egl/drivers/dri2/egl_dri2.h",
    """   /* surfaceless and device */
   struct dri_image *front;""",
    """#ifdef HAVE_INSTANTOS_PLATFORM
   struct instantos_egl_window *instantos_win;
   struct {
      struct dri_image *dri_image;
      int fd;               /* its dma-buf */
      long surface;         /* the kernel surface on it */
      int width, height;
      int age;
   } instantos_buffers[3], *instantos_back, *instantos_current;
#endif

   /* surfaceless and device */
   struct dri_image *front;""")

# Vulkan on InstantOS windows (step 7c-2c): VK_INSTANTOS_surface, a platform
# of our own (tools/mesa-port/vulkan_instantos.h, wsi_common_instantos.c).
# It goes into vk.xml so Mesa's generators give it entrypoints and an
# extension-table bit. vulkan.h defines VK_USE_PLATFORM_INSTANTOS when
# __instantos__ is (the whole Mesa build), so generated and hand-written code
# alike see the platform; a program defines it itself.
for name, source, target in (
    ("vk-instantos-header", "vulkan_instantos.h", "include/vulkan/vulkan_instantos.h"),
    ("vk-instantos-wsi", "wsi_common_instantos.c", "src/vulkan/wsi/wsi_common_instantos.c"),
):
    src = Path(__file__).resolve().parent / source
    dst = ROOT / target
    if not dst.exists() or dst.read_text(encoding="utf-8") != src.read_text(encoding="utf-8"):
        dst.write_text(src.read_text(encoding="utf-8"), encoding="utf-8")
        applied.append(name)
fix("vk-xml-platform", "src/vulkan/registry/vk.xml",
    """        <platform name="screen" protect="VK_USE_PLATFORM_SCREEN_QNX" comment="QNX Screen Graphics Subsystem"/>
""",
    """        <platform name="screen" protect="VK_USE_PLATFORM_SCREEN_QNX" comment="QNX Screen Graphics Subsystem"/>
        <platform name="instantos" protect="VK_USE_PLATFORM_INSTANTOS" comment="InstantOS compositor windows"/>
""")
fix("vk-xml-flags", "src/vulkan/registry/vk.xml",
    """        <type                                             category="bitmask">typedef <type>VkFlags</type> <name>VkHeadlessSurfaceCreateFlagsEXT</name>;</type>
""",
    """        <type                                             category="bitmask">typedef <type>VkFlags</type> <name>VkHeadlessSurfaceCreateFlagsEXT</name>;</type>
        <type                                             category="bitmask">typedef <type>VkFlags</type> <name>VkInstantOSSurfaceCreateFlagsINSTANTOS</name>;</type>
""")
fix("vk-xml-struct", "src/vulkan/registry/vk.xml",
    """            <member optional="true"><type>VkHeadlessSurfaceCreateFlagsEXT</type>   <name>flags</name></member>
        </type>
""",
    """            <member optional="true"><type>VkHeadlessSurfaceCreateFlagsEXT</type>   <name>flags</name></member>
        </type>
        <type category="struct" name="VkInstantOSSurfaceCreateInfoINSTANTOS">
            <member values="VK_STRUCTURE_TYPE_INSTANTOS_SURFACE_CREATE_INFO_INSTANTOS"><type>VkStructureType</type> <name>sType</name></member>
            <member optional="true">const <type>void</type>*            <name>pNext</name></member>
            <member optional="true"><type>VkInstantOSSurfaceCreateFlagsINSTANTOS</type>   <name>flags</name></member>
            <member><type>uint64_t</type>                         <name>window</name></member>
        </type>
""")
fix("vk-xml-command", "src/vulkan/registry/vk.xml",
    """            <proto><type>VkResult</type> <name>vkCreateHeadlessSurfaceEXT</name></proto>
            <param><type>VkInstance</type> <name>instance</name></param>
            <param>const <type>VkHeadlessSurfaceCreateInfoEXT</type>* <name>pCreateInfo</name></param>
            <param optional="true">const <type>VkAllocationCallbacks</type>* <name>pAllocator</name></param>
            <param><type>VkSurfaceKHR</type>* <name>pSurface</name></param>
        </command>
""",
    """            <proto><type>VkResult</type> <name>vkCreateHeadlessSurfaceEXT</name></proto>
            <param><type>VkInstance</type> <name>instance</name></param>
            <param>const <type>VkHeadlessSurfaceCreateInfoEXT</type>* <name>pCreateInfo</name></param>
            <param optional="true">const <type>VkAllocationCallbacks</type>* <name>pAllocator</name></param>
            <param><type>VkSurfaceKHR</type>* <name>pSurface</name></param>
        </command>
        <command successcodes="VK_SUCCESS" errorcodes="VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_OUT_OF_DEVICE_MEMORY">
            <proto><type>VkResult</type> <name>vkCreateInstantOSSurfaceINSTANTOS</name></proto>
            <param><type>VkInstance</type> <name>instance</name></param>
            <param>const <type>VkInstantOSSurfaceCreateInfoINSTANTOS</type>* <name>pCreateInfo</name></param>
            <param optional="true">const <type>VkAllocationCallbacks</type>* <name>pAllocator</name></param>
            <param><type>VkSurfaceKHR</type>* <name>pSurface</name></param>
        </command>
""")
fix("vk-xml-extension", "src/vulkan/registry/vk.xml",
    """                <command name="vkCreateHeadlessSurfaceEXT"/>
            </require>
        </extension>
""",
    """                <command name="vkCreateHeadlessSurfaceEXT"/>
            </require>
        </extension>
        <extension name="VK_INSTANTOS_surface" number="999" type="instance" depends="VK_KHR_surface" platform="instantos" author="INSTANTOS" contact="InstantOS" supported="vulkan">
            <require>
                <enum value="1"                                                 name="VK_INSTANTOS_SURFACE_SPEC_VERSION"/>
                <enum value="&quot;VK_INSTANTOS_surface&quot;"                  name="VK_INSTANTOS_SURFACE_EXTENSION_NAME"/>
                <enum offset="0" extends="VkStructureType"                      name="VK_STRUCTURE_TYPE_INSTANTOS_SURFACE_CREATE_INFO_INSTANTOS"/>
                <type name="VkInstantOSSurfaceCreateFlagsINSTANTOS"/>
                <type name="VkInstantOSSurfaceCreateInfoINSTANTOS"/>
                <command name="vkCreateInstantOSSurfaceINSTANTOS"/>
            </require>
        </extension>
""")
fix("vk-header-include", "include/vulkan/vulkan.h",
    """#ifdef VK_USE_PLATFORM_SCREEN_QNX
#include <screen/screen.h>
#include "vulkan_screen.h"
#endif
""",
    """#ifdef VK_USE_PLATFORM_SCREEN_QNX
#include <screen/screen.h>
#include "vulkan_screen.h"
#endif

#if defined(__instantos__) && !defined(VK_USE_PLATFORM_INSTANTOS)
#define VK_USE_PLATFORM_INSTANTOS 1
#endif
#ifdef VK_USE_PLATFORM_INSTANTOS
#include "vulkan_instantos.h"
#endif
""")
fix("vk-icd-platform", "include/vulkan/vk_icd.h",
    """    VK_ICD_WSI_PLATFORM_FUCHSIA,
} VkIcdWsiPlatform;""",
    """    VK_ICD_WSI_PLATFORM_FUCHSIA,
    VK_ICD_WSI_PLATFORM_INSTANTOS,
} VkIcdWsiPlatform;""")
fix("vk-icd-surface", "include/vulkan/vk_icd.h",
    """    struct wl_surface *surface;
} VkIcdSurfaceWayland;
#endif  // VK_USE_PLATFORM_WAYLAND_KHR
""",
    """    struct wl_surface *surface;
} VkIcdSurfaceWayland;
#endif  // VK_USE_PLATFORM_WAYLAND_KHR

#ifdef VK_USE_PLATFORM_INSTANTOS
typedef struct {
    VkIcdSurfaceBase base;
    uint64_t window;
} VkIcdSurfaceInstantOS;
#endif  // VK_USE_PLATFORM_INSTANTOS
""")
fix("wsi-platform-max", "src/vulkan/wsi/wsi_common.h",
    "#define VK_ICD_WSI_PLATFORM_MAX (VK_ICD_WSI_PLATFORM_METAL + 1)",
    "#define VK_ICD_WSI_PLATFORM_MAX (VK_ICD_WSI_PLATFORM_INSTANTOS + 1)")
fix("wsi-instantos-init", "src/vulkan/wsi/wsi_common.c",
    """#ifndef VK_USE_PLATFORM_WIN32_KHR
   result = wsi_headless_init_wsi(wsi, alloc, pdevice);
   if (result != VK_SUCCESS)
      goto fail;
#endif
""",
    """#ifndef VK_USE_PLATFORM_WIN32_KHR
   result = wsi_headless_init_wsi(wsi, alloc, pdevice);
   if (result != VK_SUCCESS)
      goto fail;
#endif

#ifdef VK_USE_PLATFORM_INSTANTOS
   result = wsi_instantos_init_wsi(wsi, alloc, pdevice);
   if (result != VK_SUCCESS)
      goto fail;
#endif
""")
fix("wsi-instantos-finish", "src/vulkan/wsi/wsi_common.c",
    """#ifndef VK_USE_PLATFORM_WIN32_KHR
   wsi_headless_finish_wsi(wsi, alloc);
#endif
""",
    """#ifndef VK_USE_PLATFORM_WIN32_KHR
   wsi_headless_finish_wsi(wsi, alloc);
#endif
#ifdef VK_USE_PLATFORM_INSTANTOS
   wsi_instantos_finish_wsi(wsi, alloc);
#endif
""")
fix("wsi-instantos-decl", "src/vulkan/wsi/wsi_common_private.h",
    """void wsi_headless_finish_wsi(struct wsi_device *wsi_device,
                             const VkAllocationCallbacks *alloc);
""",
    """void wsi_headless_finish_wsi(struct wsi_device *wsi_device,
                             const VkAllocationCallbacks *alloc);

#ifdef VK_USE_PLATFORM_INSTANTOS
VkResult wsi_instantos_init_wsi(struct wsi_device *wsi_device,
                                const VkAllocationCallbacks *alloc,
                                VkPhysicalDevice physical_device);

void wsi_instantos_finish_wsi(struct wsi_device *wsi_device,
                              const VkAllocationCallbacks *alloc);
#endif
""")
fix("wsi-instantos-meson", "src/vulkan/wsi/meson.build",
    """if with_platform_macos
  files_vulkan_wsi += files('wsi_common_metal.c', 'wsi_common_metal_layer.m')
endif
""",
    """if with_platform_macos
  files_vulkan_wsi += files('wsi_common_metal.c', 'wsi_common_metal_layer.m')
endif

if with_platform_instantos
  files_vulkan_wsi += files('wsi_common_instantos.c')
endif
""")
fix("anv-instantos-surface", "src/intel/vulkan/anv_instance.c",
    """   .EXT_headless_surface                     = true,
""",
    """   .EXT_headless_surface                     = true,
#ifdef VK_USE_PLATFORM_INSTANTOS
   .INSTANTOS_surface                        = true,
#endif
""")

print("patch-mesa:", ", ".join(applied) if applied else "nothing to do")
