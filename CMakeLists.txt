cmake_minimum_required(VERSION 3.14)

project(spscring
    VERSION 0.1.0
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

if(SPSCRING_SANITIZE)
    target_compile_options(spscring INTERFACE -fsanitize=${SPSCRING_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(spscring INTERFACE -fsanitize=${SPSCRING_SANITIZE})
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

include(GNUInstallDirs)
install(TARGETS spscring EXPORT spscringTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT spscringTargets
    NAMESPACE spscring::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spscring
)
