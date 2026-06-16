option(INSTANTOS_ENABLE_TCC "Build the optional TinyCC port" OFF)
set(INSTANTOS_TCC_SOURCE_DIR "${CMAKE_SOURCE_DIR}/outside/iUserApps/outside/tinycc" CACHE PATH "Path to a TinyCC source checkout")
set(INSTANTOS_TCC_SYSROOT_DIR "${CMAKE_BINARY_DIR}/tcc-sysroot" CACHE PATH "Path to the generated TinyCC InstantOS sysroot")
set(INSTANTOS_TCC_BUILD_DIR "${CMAKE_BINARY_DIR}/tcc-build" CACHE PATH "Path to the TinyCC build directory")

add_custom_target(tcc-sysroot
    COMMAND ${CMAKE_COMMAND} -E env
        BUILD_DIR=${CMAKE_BINARY_DIR}
        TCC_SYSROOT=${INSTANTOS_TCC_SYSROOT_DIR}
        ${CMAKE_SOURCE_DIR}/tools/build-tcc-sysroot.sh
    DEPENDS instant_c_crt0 ilibcxx ld-instantos
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Preparing TinyCC InstantOS sysroot"
    VERBATIM
)

if(INSTANTOS_ENABLE_TCC)
    if(NOT EXISTS "${INSTANTOS_TCC_SOURCE_DIR}/configure")
        message(FATAL_ERROR "TinyCC source not found at ${INSTANTOS_TCC_SOURCE_DIR}; run tools/fetch-tcc.sh or set INSTANTOS_TCC_SOURCE_DIR")
    endif()

    add_custom_target(tcc
        COMMAND ${CMAKE_COMMAND} -E env
            BUILD_DIR=${CMAKE_BINARY_DIR}
            TCC_SOURCE_DIR=${INSTANTOS_TCC_SOURCE_DIR}
            TCC_BUILD_DIR=${INSTANTOS_TCC_BUILD_DIR}
            TCC_SYSROOT=${INSTANTOS_TCC_SYSROOT_DIR}
            TCC_NATIVE_BUILD=1
            ${CMAKE_SOURCE_DIR}/tools/build-tcc.sh
        DEPENDS tcc-sysroot
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Building optional TinyCC InstantOS port"
        VERBATIM
    )
endif()
