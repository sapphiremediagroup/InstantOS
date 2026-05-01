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

add_custom_command(
    OUTPUT ${INITRD_IMG}
    COMMAND ${MKINITRD_EXE}
        ${INITRD_IMG}
        input-manager.exe:${CMAKE_BINARY_DIR}/input-manager.exe
        cube.exe:${CMAKE_BINARY_DIR}/cube.exe
        graphics-compositor.exe:${CMAKE_BINARY_DIR}/graphics-compositor.exe
        storage-manager.exe:${CMAKE_BINARY_DIR}/storage-manager.exe
        process-manager.exe:${CMAKE_BINARY_DIR}/process-manager.exe
        font-manager.exe:${CMAKE_BINARY_DIR}/font-manager.exe
        session-manager.exe:${CMAKE_BINARY_DIR}/session-manager.exe
        login.exe:${CMAKE_BINARY_DIR}/login.exe
        desktop-shell.exe:${CMAKE_BINARY_DIR}/desktop-shell.exe
        background-switcher.exe:${CMAKE_BINARY_DIR}/background-switcher.exe
        file-browser.exe:${CMAKE_BINARY_DIR}/file-browser.exe
        terminal.exe:${CMAKE_BINARY_DIR}/terminal.exe
        JetBrainsMono-Regular.ttf:${CMAKE_SOURCE_DIR}/assets/fonts/JetBrainsMono-Regular.ttf
        cursors/default.png:${CMAKE_SOURCE_DIR}/assets/cursors/default.png
        backgrounds/instantos_1.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_1.png
        backgrounds/instantos_1_mono.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_1_mono.png
        backgrounds/instantos_2.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_2.png
        backgrounds/instantos_2_mono.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_2_mono.png
        backgrounds/instantos_3_color.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_3_color.png
        backgrounds/instantos_3_mono.png:${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_3_mono.png
        kernel.map:${CMAKE_BINARY_DIR}/kernel.map
    DEPENDS mkInitrd_proj cube.exe input-manager.exe graphics-compositor.exe storage-manager.exe process-manager.exe font-manager.exe session-manager.exe login.exe desktop-shell.exe background-switcher.exe file-browser.exe terminal.exe ${CMAKE_SOURCE_DIR}/assets/fonts/JetBrainsMono-Regular.ttf ${CMAKE_SOURCE_DIR}/assets/cursors/default.png ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_1.png ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_1_mono.png ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_2.png ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_2_mono.png ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_3_color.png ${CMAKE_SOURCE_DIR}/assets/backgrounds/instantos_3_mono.png
    VERBATIM
)

add_custom_target(make_initrd ALL DEPENDS ${INITRD_IMG})
add_dependencies(make_initrd input-manager.exe graphics-compositor.exe storage-manager.exe process-manager.exe font-manager.exe session-manager.exe login.exe desktop-shell.exe background-switcher.exe file-browser.exe terminal.exe cube.exe)

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
    DEPENDS BOOTX64.EFI INSTANTOS.EFI make_initrd
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ISO_EFI_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/BOOTX64.EFI" "${ISO_EFI_DIR}/BOOTX64.EFI"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/INSTANTOS.EFI" "${ISO_EFI_DIR}/INSTANTOS.EFI"
    COMMAND ${CMAKE_COMMAND} -E copy "${INITRD_IMG}" "${ISO_EFI_DIR}/INITRD"
)

add_custom_command(
    OUTPUT ${EFI_IMG}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ISO_EFI_DIR}"
    COMMAND dd if=/dev/zero of=${EFI_IMG} bs=1M count=10

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

    DEPENDS BOOTX64.EFI INSTANTOS.EFI make_initrd
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
    COMMAND dd if=/dev/zero of=${INPUT_PROBE_EFI_IMG} bs=1M count=10

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
