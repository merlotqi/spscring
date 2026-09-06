include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

function(spscring_gtest_executable name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE spscring GTest::gtest_main)
    set_target_properties(${name} PROPERTIES FOLDER "Tests")
    spscring_enable_warnings(${name})
    # Each target gets its own discovery scratch directory: gtest_discover_tests
    # derives its intermediate JSON file name from the (empty) extra args/executor,
    # so parallel Ninja discovery steps would otherwise race on one shared file.
    # TEST_JSON_OUTPUT_DIR needs CMake >= 3.24.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
        set(discovery_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}_discovery")
        file(MAKE_DIRECTORY "${discovery_dir}")
        gtest_discover_tests(${name} TEST_JSON_OUTPUT_DIR "${discovery_dir}")
    else()
        gtest_discover_tests(${name})
    endif()
endfunction()

spscring_gtest_executable(spscring_fixed_ring_test fixed_ring_test.cpp)
spscring_gtest_executable(spscring_varlen_ring_test varlen_ring_test.cpp)
spscring_gtest_executable(spscring_control_block_test control_block_test.cpp)
spscring_gtest_executable(spscring_atomic_wait_test atomic_wait_test.cpp)

