cmake_minimum_required(VERSION 3.14)

# Reusable build helpers (warnings, sanitizers, version, packaging).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(spscring-version)
include(spscring-warnings)
include(spscring-sanitizers)

project(spscring
    VERSION ${SPSCRING_VERSION}
    DESCRIPTION "Single-producer/single-consumer lock-free ring buffers for shared-memory IPC"
    LANGUAGES CXX
)

add_library(spscring INTERFACE)
add_library(spscring::spscring ALIAS spscring)

target_compile_features(spscring INTERFACE cxx_std_17)
target_include_directories(spscring INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    # The public headers are UTF-8 and contain non-ASCII comment characters; make
    # sure consumers compiling on MSVC interpret them as UTF-8 instead of the
    # active code page (which would raise C4819 and could corrupt them).
    target_compile_options(spscring INTERFACE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

if(SPSCRING_SANITIZE)
    spscring_enable_sanitizers(spscring "${SPSCRING_SANITIZE}")
endif()

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(SPSCRING_IS_TOP_LEVEL ON)
else()
    set(SPSCRING_IS_TOP_LEVEL OFF)
endif()

option(SPSCRING_BUILD_TESTS "Build spscring unit tests" ${SPSCRING_IS_TOP_LEVEL})
option(SPSCRING_BUILD_BENCHMARKS "Build spscring benchmarks" OFF)
option(SPSCRING_BUILD_EXAMPLES "Build spscring examples" ${SPSCRING_IS_TOP_LEVEL})

if(SPSCRING_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SPSCRING_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(SPSCRING_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---- Install / packaging ----

include(GNUInstallDirs)

install(TARGETS spscring EXPORT spscringTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT spscringTargets
    NAMESPACE spscring::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spscring
)

# find_package(spscring CONFIG) support.
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spscring-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/spscring-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spscring
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/spscring-config-version.cmake
    VERSION ${SPSCRING_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/spscring-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/spscring-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spscring
)

# pkg-config (.pc) support.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spscring.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/spscring.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/spscring.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
