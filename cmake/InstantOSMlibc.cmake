option(INSTANTOS_ENABLE_MLIBC "Build the optional mlibc port" OFF)
set(INSTANTOS_MLIBC_SOURCE_DIR "${CMAKE_SOURCE_DIR}/outside/iUserApps/outside/mlibc" CACHE PATH "Path to an mlibc source checkout")
set(INSTANTOS_MLIBC_BUILD_DIR "${CMAKE_BINARY_DIR}/mlibc-build" CACHE PATH "Path to the mlibc Meson build directory")
set(INSTANTOS_MLIBC_INSTALL_DIR "${CMAKE_BINARY_DIR}/mlibc-root" CACHE PATH "Path to the mlibc install prefix")

if(INSTANTOS_ENABLE_MLIBC)
    find_program(MESON_EXECUTABLE meson REQUIRED)
    find_program(NINJA_EXECUTABLE ninja REQUIRED)

    if(NOT EXISTS "${INSTANTOS_MLIBC_SOURCE_DIR}/meson.build")
        message(FATAL_ERROR "mlibc source not found at ${INSTANTOS_MLIBC_SOURCE_DIR}; run tools/fetch-mlibc.sh or set INSTANTOS_MLIBC_SOURCE_DIR")
    endif()

    add_custom_target(mlibc
        COMMAND ${CMAKE_COMMAND} -E env
            MLIBC_SOURCE_DIR=${INSTANTOS_MLIBC_SOURCE_DIR}
            MLIBC_BUILD_DIR=${INSTANTOS_MLIBC_BUILD_DIR}
            MLIBC_INSTALL_DIR=${INSTANTOS_MLIBC_INSTALL_DIR}
            MESON=${MESON_EXECUTABLE}
            NINJA=${NINJA_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/build-mlibc.sh
        BYPRODUCTS ${INSTANTOS_MLIBC_INSTALL_DIR}/lib/libc.so
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Building optional mlibc InstantOS port"
        VERBATIM
    )
endif()
