cmake_minimum_required(VERSION 3.20)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
project(InstantOS C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)

set(CMAKE_ASM_NASM_FLAGS "" CACHE STRING "" FORCE)
set(CMAKE_ASM_NASM_FLAGS_DEBUG "" CACHE STRING "" FORCE)
set(CMAKE_ASM_NASM_FLAGS_RELEASE "" CACHE STRING "" FORCE)

enable_language(ASM_NASM)

foreach(lang C CXX)
    foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set(CMAKE_${lang}_FLAGS_${cfg} "" CACHE STRING "" FORCE)
    endforeach()
endforeach()
set(CMAKE_C_FLAGS   "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "" CACHE STRING "" FORCE)

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_EXE_LINKER_FLAGS_${cfg} "" CACHE STRING "" FORCE)
endforeach()
set(CMAKE_EXE_LINKER_FLAGS "" CACHE STRING "" FORCE)

set(BASE_CXX_FLAGS
    -fno-rtti
    -fno-exceptions
    -fshort-wchar
    -ffreestanding
    -fno-threadsafe-statics
    -fno-inline
    -fno-use-cxa-atexit
    -fno-omit-frame-pointer
    -nodefaultlibs
    -nostdinc++
    -Xclang --dependent-lib=
)

set(UEFI_CXX_FLAGS
    -fno-rtti
    -fno-exceptions
    -target x86_64-unknown-windows
)

set(SIZE_CXX_FLAGS
    -Oz
    -ffunction-sections
    -fdata-sections
    -fomit-frame-pointer
)

set(SIZE_LINK_OPTIONS
    -Wl,/OPT:REF
    -Wl,/OPT:ICF
)

set(EFI_LINK_OPTIONS
    -target x86_64-unknown-windows
    -nostdlib
    -nostartfiles
    -nodefaultlibs
    -fuse-ld=lld-link
    -Wl,/nodefaultlib
)

add_subdirectory(outside/iUserApps)

include(ExternalProject)
include(cmake/InstantOSMlibc.cmake)
include(cmake/InstantOSTcc.cmake)

set(MKINITRD_INSTALL_DIR ${CMAKE_BINARY_DIR}/mkInitrd_host)

ExternalProject_Add(mkInitrd_proj
    SOURCE_DIR  ${CMAKE_SOURCE_DIR}/tools/mkInitrd
    BINARY_DIR  ${CMAKE_BINARY_DIR}/mkInitrd_build
    INSTALL_DIR ${MKINITRD_INSTALL_DIR}
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DCMAKE_BUILD_TYPE=MinSizeRel
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/mkInitrd_build/mkInitrd
)

set(MKINITRD_EXE ${CMAKE_BINARY_DIR}/mkInitrd_build/mkInitrd)

set(INITRD_IMG ${CMAKE_BINARY_DIR}/initrd.img)

set(TCC_INITRD_ENTRIES)
set(TCC_INITRD_DEPENDS)
if(INSTANTOS_ENABLE_TCC)
    list(APPEND TCC_INITRD_ENTRIES
        bin/tcc:${CMAKE_BINARY_DIR}/tcc
        bin/tcc-hello.c:${CMAKE_SOURCE_DIR}/tools/tcc/hello.c
        lib/crt0.o:${INSTANTOS_TCC_SYSROOT_DIR}/lib/crt0.o
        lib/crt1.o:${INSTANTOS_TCC_SYSROOT_DIR}/lib/crt1.o
        lib/Scrt1.o:${INSTANTOS_TCC_SYSROOT_DIR}/lib/Scrt1.o
        lib/crti.o:${INSTANTOS_TCC_SYSROOT_DIR}/lib/crti.o
        lib/crtn.o:${INSTANTOS_TCC_SYSROOT_DIR}/lib/crtn.o
        lib/libc.so:${INSTANTOS_TCC_SYSROOT_DIR}/lib/libc.so
        lib/tcc/libtcc1.a:${INSTANTOS_TCC_SYSROOT_DIR}/lib/tcc/libtcc1.a
    )
    list(APPEND TCC_INITRD_DEPENDS
        tcc
        ${CMAKE_SOURCE_DIR}/tools/tcc/hello.c
        ${INSTANTOS_TCC_SYSROOT_DIR}/lib/crt0.o
        ${INSTANTOS_TCC_SYSROOT_DIR}/lib/crt1.o
        ${INSTANTOS_TCC_SYSROOT_DIR}/lib/Scrt1.o
        ${INSTANTOS_TCC_SYSROOT_DIR}/lib/crti.o
        ${INSTANTOS_TCC_SYSROOT_DIR}/lib/crtn.o
        ${INSTANTOS_TCC_SYSROOT_DIR}/lib/libc.so
        ${INSTANTOS_TCC_SYSROOT_DIR}/lib/tcc/libtcc1.a
    )

    file(GLOB_RECURSE TCC_INITRD_HEADERS CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/outside/iUserApps/outside/ilibcxx/include/*.h
    )
    foreach(header_file IN LISTS TCC_INITRD_HEADERS)
        if(NOT IS_DIRECTORY ${header_file})
            file(RELATIVE_PATH header_rel
                ${CMAKE_SOURCE_DIR}/outside/iUserApps/outside/ilibcxx/include
                ${header_file}
            )
            # libc headers live in the system include path (/include). tcc's own
            # compiler headers (stdarg.h/stddef.h/etc.) must NOT be overwritten by
            # these, so they are staged separately into /lib/tcc/include below.
            set(header_initrd_name "include/${header_rel}")
            string(LENGTH "${header_initrd_name}" header_initrd_name_length)
            if(header_initrd_name_length GREATER_EQUAL 64)
                message(FATAL_ERROR "TCC initrd header path is too long: ${header_initrd_name}")
            endif()
            list(APPEND TCC_INITRD_ENTRIES
                ${header_initrd_name}:${header_file}
            )
            list(APPEND TCC_INITRD_DEPENDS ${header_file})
        endif()
    endforeach()

    # tcc's own compiler-builtin headers (stdarg.h, stddef.h, varargs.h, ...).
    # These define tcc-compatible va_list/etc. and must take precedence in
    # tcc's private include dir (/lib/tcc/include, first on the search path).
    file(GLOB TCC_NATIVE_HEADERS CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/outside/iUserApps/outside/tinycc/include/*.h
    )
    foreach(header_file IN LISTS TCC_NATIVE_HEADERS)
        if(NOT IS_DIRECTORY ${header_file})
            get_filename_component(header_name ${header_file} NAME)
            list(APPEND TCC_INITRD_ENTRIES
                lib/tcc/include/${header_name}:${header_file}
            )
            list(APPEND TCC_INITRD_DEPENDS ${header_file})
        endif()
    endforeach()
endif()

# Optional GNU bash + mlibc runtime. These are produced by tools/build-mlibc.sh
# and tools/build-bash.sh (not by this CMake build), so they are bundled into
# the initrd only when present.
set(BASH_INITRD_ENTRIES "")
set(BASH_INITRD_DEPENDS "")
set(INSTANTOS_BASH_BINARY "${CMAKE_BINARY_DIR}/bash")
set(INSTANTOS_MLIBC_LIB_DIR "${CMAKE_BINARY_DIR}/mlibc-root/lib")
if(EXISTS "${INSTANTOS_BASH_BINARY}" AND EXISTS "${INSTANTOS_MLIBC_LIB_DIR}/libc.so")
    list(APPEND BASH_INITRD_ENTRIES
        bin/bash:${INSTANTOS_BASH_BINARY}
        lib/mlibc/ld-instantos.so:${INSTANTOS_MLIBC_LIB_DIR}/ld-instantos.so
        lib/mlibc/libc.so:${INSTANTOS_MLIBC_LIB_DIR}/libc.so
        lib/mlibc/libdl.so:${INSTANTOS_MLIBC_LIB_DIR}/libdl.so
    )
    list(APPEND BASH_INITRD_DEPENDS
        ${INSTANTOS_BASH_BINARY}
        ${INSTANTOS_MLIBC_LIB_DIR}/libc.so
        ${INSTANTOS_MLIBC_LIB_DIR}/libdl.so
        ${INSTANTOS_MLIBC_LIB_DIR}/ld-instantos.so
    )
    message(STATUS "Bundling GNU bash + mlibc runtime into initrd")
else()
    message(STATUS "bash/mlibc artifacts not found; skipping (run tools/build-mlibc.sh + tools/build-bash.sh)")
endif()

# Optional NetSurf framebuffer browser. Produced by tools/build-netsurf*.sh
# (not by this CMake build), so it is bundled into the initrd only when the
# nsfb binary and its mlibc runtime + resources are present. NetSurf links
# against the mlibc shared libraries (incl. libm) and reads its resources from
# /netsurf/res (the path compiled in via NETSURF_FRAMEBUFFER_RESOURCES).
set(NETSURF_INITRD_ENTRIES "")
set(NETSURF_INITRD_DEPENDS "")
set(INSTANTOS_NETSURF_BINARY "${CMAKE_BINARY_DIR}/netsurf-port/netsurf-3.11/nsfb")
set(INSTANTOS_NETSURF_RES "${CMAKE_BINARY_DIR}/netsurf-port/netsurf-3.11/frontends/framebuffer/res")
if(EXISTS "${INSTANTOS_NETSURF_BINARY}" AND EXISTS "${INSTANTOS_MLIBC_LIB_DIR}/libc.so")
    list(APPEND NETSURF_INITRD_ENTRIES
        bin/nsfb:${INSTANTOS_NETSURF_BINARY}
        lib/mlibc/ld-instantos.so:${INSTANTOS_MLIBC_LIB_DIR}/ld-instantos.so
        lib/mlibc/libc.so:${INSTANTOS_MLIBC_LIB_DIR}/libc.so
        lib/mlibc/libm.so:${INSTANTOS_MLIBC_LIB_DIR}/libm.so
        lib/mlibc/libdl.so:${INSTANTOS_MLIBC_LIB_DIR}/libdl.so
        lib/mlibc/libpthread.so:${INSTANTOS_MLIBC_LIB_DIR}/libpthread.so
    )
    list(APPEND NETSURF_INITRD_DEPENDS
        ${INSTANTOS_NETSURF_BINARY}
        ${INSTANTOS_MLIBC_LIB_DIR}/libc.so
    )
    # Bundle the NetSurf resource tree under /netsurf/res preserving structure.
    # FOLLOW_SYMLINKS is required: NetSurf ships many top-level resources
    # (default.css, welcome.html, netsurf.png, ...) as symlinks into the shared
    # resources/ tree. Without following them the browser boots without its
    # stylesheets/images and the welcome page fails to load.
    file(GLOB_RECURSE NETSURF_RES_FILES FOLLOW_SYMLINKS "${INSTANTOS_NETSURF_RES}/*")
    foreach(res_file ${NETSURF_RES_FILES})
        if(IS_DIRECTORY "${res_file}")
            continue()
        endif()
        # Resolve symlinks so the initrd entry carries the real file contents.
        get_filename_component(res_real "${res_file}" REALPATH)
        if(NOT EXISTS "${res_real}" OR IS_DIRECTORY "${res_real}")
            continue()
        endif()
        file(RELATIVE_PATH res_rel "${INSTANTOS_NETSURF_RES}" "${res_file}")
        list(APPEND NETSURF_INITRD_ENTRIES "netsurf/res/${res_rel}:${res_real}")
        list(APPEND NETSURF_INITRD_DEPENDS "${res_real}")
    endforeach()
    message(STATUS "Bundling NetSurf framebuffer browser into initrd")
else()
    message(STATUS "NetSurf artifacts not found; skipping (run tools/build-netsurf*.sh)")
endif()

# CA certificate bundle for TLS (https://). Placed under the already-mounted
# /netsurf subtree at /netsurf/etc/cacert.pem; NetSurf is configured to use it
# for full certificate-chain validation. Only bundled when fetched (run
# tools/fetch-cacert.sh).
set(INSTANTOS_CACERT "${CMAKE_BINARY_DIR}/cacert/cacert.pem")
if(EXISTS "${INSTANTOS_CACERT}")
    list(APPEND NETSURF_INITRD_ENTRIES "netsurf/etc/cacert.pem:${INSTANTOS_CACERT}")
    list(APPEND NETSURF_INITRD_DEPENDS "${INSTANTOS_CACERT}")
    message(STATUS "Bundling CA certificate bundle into initrd (/netsurf/etc/cacert.pem)")
else()
    message(STATUS "CA bundle not found; https:// will fail cert validation (run tools/fetch-cacert.sh)")
endif()

add_custom_command(
    OUTPUT ${INITRD_IMG}
    COMMAND ${MKINITRD_EXE}
        ${INITRD_IMG}
        bin/input-manager:${CMAKE_BINARY_DIR}/input-manager
        bin/cube:${CMAKE_BINARY_DIR}/cube
        bin/graphics-compositor:${CMAKE_BINARY_DIR}/graphics-compositor
        bin/storage-manager:${CMAKE_BINARY_DIR}/storage-manager
        bin/process-manager:${CMAKE_BINARY_DIR}/process-manager
        bin/network-manager:${CMAKE_BINARY_DIR}/network-manager
        bin/font-manager:${CMAKE_BINARY_DIR}/font-manager
        bin/session-manager:${CMAKE_BINARY_DIR}/session-manager
        bin/login:${CMAKE_BINARY_DIR}/login
        bin/desktop-shell:${CMAKE_BINARY_DIR}/desktop-shell
        bin/background-switcher:${CMAKE_BINARY_DIR}/background-switcher
        bin/file-browser:${CMAKE_BINARY_DIR}/file-browser
        bin/terminal:${CMAKE_BINARY_DIR}/terminal
        bin/net-smoke:${CMAKE_BINARY_DIR}/net-smoke
        ${TCC_INITRD_ENTRIES}
        bin/JetBrainsMono-Regular.ttf:${CMAKE_SOURCE_DIR}/assets/fonts/JetBrainsMono-Regular.ttf
        bin/StackSansNotch-Bold.ttf:${CMAKE_SOURCE_DIR}/assets/fonts/StackSansNotch-Bold.ttf
        bin/StackSansNotch-SemiBold.ttf:${CMAKE_SOURCE_DIR}/assets/fonts/StackSansNotch-SemiBold.ttf
        bin/StackSansNotch-Regular.ttf:${CMAKE_SOURCE_DIR}/assets/fonts/StackSansNotch-Regular.ttf
        bin/StackSansNotch-ExtraLight.ttf:${CMAKE_SOURCE_DIR}/assets/fonts/StackSansNotch-ExtraLight.ttf
        bin/cursors/default.png:${CMAKE_SOURCE_DIR}/assets/cursors/default.png
        bin/images/InstantOS_sapph.png:${CMAKE_SOURCE_DIR}/assets/images/InstantOS_sapph.png
        bin/images/no_itnernet.png:${CMAKE_SOURCE_DIR}/assets/images/no_itnernet.png
        bin/images/no_sound.png:${CMAKE_SOURCE_DIR}/assets/images/no_sound.png
        bin/images/close.png:${CMAKE_SOURCE_DIR}/assets/images/close.png
        bin/images/minimize.png:${CMAKE_SOURCE_DIR}/assets/images/minimize.png
        bin/images/resize.png:${CMAKE_SOURCE_DIR}/assets/images/resize.png
        bin/images/folder.svg:${CMAKE_SOURCE_DIR}/assets/images/folder.svg
        bin/images/file.svg:${CMAKE_SOURCE_DIR}/assets/images/file.svg
        bin/backgrounds/instantos_1.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_1.png
        bin/backgrounds/instantos_1_mono.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_1_mono.png
        bin/backgrounds/instantos_2.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_2.png
        bin/backgrounds/instantos_2_mono.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_2_mono.png
        bin/backgrounds/instantos_3_color.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_3_color.png
        bin/backgrounds/instantos_3_mono.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_3_mono.png
        bin/kernel.map:${CMAKE_BINARY_DIR}/kernel.map
        lib/ld-instantos.so:${CMAKE_BINARY_DIR}/ld-instantos.so
        lib/libinstant.so:${CMAKE_BINARY_DIR}/libinstant.so
        lib/libm.so.6:${CMAKE_BINARY_DIR}/libinstant.so
        ${BASH_INITRD_ENTRIES}
        ${NETSURF_INITRD_ENTRIES}
    DEPENDS mkInitrd_proj INSTANTOS.EFI cube input-manager graphics-compositor storage-manager process-manager network-manager font-manager session-manager login desktop-shell background-switcher file-browser terminal net-smoke ld-instantos ilibcxx ${TCC_INITRD_DEPENDS} ${CMAKE_SOURCE_DIR}/assets/fonts/JetBrainsMono-Regular.ttf ${CMAKE_SOURCE_DIR}/assets/fonts/StackSansNotch-Bold.ttf ${CMAKE_SOURCE_DIR}/assets/fonts/StackSansNotch-SemiBold.ttf ${CMAKE_SOURCE_DIR}/assets/fonts/StackSansNotch-Regular.ttf ${CMAKE_SOURCE_DIR}/assets/fonts/StackSansNotch-ExtraLight.ttf ${CMAKE_SOURCE_DIR}/assets/cursors/default.png ${CMAKE_SOURCE_DIR}/assets/images/InstantOS_sapph.png ${CMAKE_SOURCE_DIR}/assets/images/no_itnernet.png ${CMAKE_SOURCE_DIR}/assets/images/no_sound.png ${CMAKE_SOURCE_DIR}/assets/images/close.png ${CMAKE_SOURCE_DIR}/assets/images/minimize.png ${CMAKE_SOURCE_DIR}/assets/images/resize.png ${CMAKE_SOURCE_DIR}/assets/images/folder.svg ${CMAKE_SOURCE_DIR}/assets/images/file.svg ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_1.png ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_1_mono.png ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_2.png ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_2_mono.png ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_3_color.png ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_3_mono.png ${BASH_INITRD_DEPENDS} ${NETSURF_INITRD_DEPENDS}
    VERBATIM
)

add_custom_target(make_initrd ALL DEPENDS ${INITRD_IMG})
add_dependencies(make_initrd INSTANTOS.EFI input-manager graphics-compositor storage-manager process-manager network-manager font-manager session-manager login desktop-shell background-switcher file-browser terminal cube net-smoke ld-instantos ilibcxx)

add_subdirectory(outside/iboot)

file(GLOB_RECURSE INSTANTOS_SOURCES
    CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.asm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
)

function(configure_instantos_kernel target_name map_name)
    target_link_libraries(${target_name} PRIVATE
        iboot::headers
    )

    target_include_directories(${target_name} PRIVATE
        include
    )

    target_compile_options(${target_name} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${BASE_CXX_FLAGS}>
        $<$<COMPILE_LANGUAGE:CXX>:${UEFI_CXX_FLAGS}>
        $<$<COMPILE_LANGUAGE:CXX>:${SIZE_CXX_FLAGS}>
    )

    target_link_options(${target_name} PRIVATE
        ${SIZE_LINK_OPTIONS}
        -Wl,/Map:${map_name}
    )

    target_link_options(${target_name} PRIVATE
        ${EFI_LINK_OPTIONS}
        -Wl,-subsystem:efi_application
        -Wl,-entry:InstantOS
    )

    set_target_properties(${target_name} PROPERTIES SUFFIX "")
endfunction()

add_executable(INSTANTOS.EFI ${INSTANTOS_SOURCES})
configure_instantos_kernel(INSTANTOS.EFI kernel.map)
target_compile_definitions(INSTANTOS.EFI PRIVATE $<$<CONFIG:Debug>:INSTANTOS_DEBUG=0>)

add_executable(INSTANTOS_INPUT_PROBE.EFI ${INSTANTOS_SOURCES})
configure_instantos_kernel(INSTANTOS_INPUT_PROBE.EFI kernel.input_probe.map)
target_compile_definitions(INSTANTOS_INPUT_PROBE.EFI PRIVATE INPUT_PROBE_ONLY=1)

# =========================
# ISO staging
# =========================
set(ISO_ROOT ${CMAKE_BINARY_DIR}/iso)
set(ISO_EFI_DIR ${ISO_ROOT}/EFI/BOOT)
set(EFI_IMG ${ISO_EFI_DIR}/efiboot.img)
set(INPUT_PROBE_ISO_ROOT ${CMAKE_BINARY_DIR}/iso-input-probe)
set(INPUT_PROBE_ISO_EFI_DIR ${INPUT_PROBE_ISO_ROOT}/EFI/BOOT)
set(INPUT_PROBE_EFI_IMG ${INPUT_PROBE_ISO_EFI_DIR}/efiboot.img)

add_custom_target(copy ALL
    DEPENDS BOOTX64.EFI INSTANTOS.EFI make_initrd ${INITRD_IMG}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ISO_EFI_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/BOOTX64.EFI" "${ISO_EFI_DIR}/BOOTX64.EFI"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/INSTANTOS.EFI" "${ISO_EFI_DIR}/INSTANTOS.EFI"
    COMMAND ${CMAKE_COMMAND} -E copy "${INITRD_IMG}" "${ISO_EFI_DIR}/INITRD"
)

add_custom_command(
    OUTPUT ${EFI_IMG}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ISO_EFI_DIR}"
    COMMAND dd if=/dev/zero of=${EFI_IMG} bs=1M count=48

    COMMAND mformat -i ${EFI_IMG} ::

    COMMAND mmd -i ${EFI_IMG} ::/EFI
    COMMAND mmd -i ${EFI_IMG} ::/EFI/BOOT

    COMMAND mcopy -i ${EFI_IMG}
            ${CMAKE_BINARY_DIR}/BOOTX64.EFI
            ::/EFI/BOOT/BOOTX64.EFI
    COMMAND mcopy -i ${EFI_IMG}
            ${CMAKE_BINARY_DIR}/INSTANTOS.EFI
            ::/EFI/BOOT/INSTANTOS.EFI
    COMMAND mcopy -i ${EFI_IMG}
            ${INITRD_IMG}
            ::/EFI/BOOT/INITRD

    DEPENDS BOOTX64.EFI INSTANTOS.EFI make_initrd ${INITRD_IMG}
    VERBATIM
)

add_custom_target(input_probe_copy
    DEPENDS BOOTX64.EFI INSTANTOS_INPUT_PROBE.EFI
    COMMAND ${CMAKE_COMMAND} -E make_directory "${INPUT_PROBE_ISO_EFI_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/BOOTX64.EFI" "${INPUT_PROBE_ISO_EFI_DIR}/BOOTX64.EFI"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/INSTANTOS_INPUT_PROBE.EFI" "${INPUT_PROBE_ISO_EFI_DIR}/INSTANTOS.EFI"
)

add_custom_command(
    OUTPUT ${INPUT_PROBE_EFI_IMG}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${INPUT_PROBE_ISO_EFI_DIR}"
    COMMAND dd if=/dev/zero of=${INPUT_PROBE_EFI_IMG} bs=1M count=48

    COMMAND mformat -i ${INPUT_PROBE_EFI_IMG} ::

    COMMAND mmd -i ${INPUT_PROBE_EFI_IMG} ::/EFI
    COMMAND mmd -i ${INPUT_PROBE_EFI_IMG} ::/EFI/BOOT

    COMMAND mcopy -i ${INPUT_PROBE_EFI_IMG}
            ${CMAKE_BINARY_DIR}/BOOTX64.EFI
            ::/EFI/BOOT/BOOTX64.EFI
    COMMAND mcopy -i ${INPUT_PROBE_EFI_IMG}
            ${CMAKE_BINARY_DIR}/INSTANTOS_INPUT_PROBE.EFI
            ::/EFI/BOOT/INSTANTOS.EFI

    DEPENDS BOOTX64.EFI INSTANTOS_INPUT_PROBE.EFI
    VERBATIM
)

# =========================
# ISO generation
# =========================
if(WIN32)
    add_custom_target(iso
        DEPENDS copy ${EFI_IMG}
        COMMAND oscdimg.exe
            -m -o -u2 -udfver102
            -bootdata:1#pEF,e,b${EFI_IMG}
            ${ISO_ROOT}
            ${CMAKE_BINARY_DIR}/iboot.iso
    )
    add_custom_target(input_probe_iso
        DEPENDS input_probe_copy ${INPUT_PROBE_EFI_IMG}
        COMMAND oscdimg.exe
            -m -o -u2 -udfver102
            -bootdata:1#pEF,e,b${INPUT_PROBE_EFI_IMG}
            ${INPUT_PROBE_ISO_ROOT}
            ${CMAKE_BINARY_DIR}/input-probe.iso
    )
else()
    add_custom_target(iso
        DEPENDS copy ${EFI_IMG}
        COMMAND xorriso -as mkisofs
            -R -J -joliet-long
            -iso-level 3
            -eltorito-alt-boot
            -e EFI/BOOT/efiboot.img
            -no-emul-boot
            -o ${CMAKE_BINARY_DIR}/iboot.iso
            ${ISO_ROOT}
    )
    add_custom_target(input_probe_iso
        DEPENDS input_probe_copy ${INPUT_PROBE_EFI_IMG}
        COMMAND xorriso -as mkisofs
            -R -J -joliet-long
            -iso-level 3
            -eltorito-alt-boot
            -e EFI/BOOT/efiboot.img
            -no-emul-boot
            -o ${CMAKE_BINARY_DIR}/input-probe.iso
            ${INPUT_PROBE_ISO_ROOT}
    )
endif()
