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
    gtest_discover_tests(${name})
endfunction()

spscring_gtest_executable(spscring_fixed_ring_test fixed_ring_test.cpp)
spscring_gtest_executable(spscring_varlen_ring_test varlen_ring_test.cpp)
spscring_gtest_executable(spscring_control_block_test control_block_test.cpp)
