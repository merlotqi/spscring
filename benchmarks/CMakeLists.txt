include(FetchContent)

FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.4
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

include(CTest)

foreach(bench fixed_ring varlen_ring)
  add_executable(spscring_bench_${bench} ${bench}_benchmark.cpp)
  target_link_libraries(spscring_bench_${bench} PRIVATE spscring benchmark::benchmark_main)
  set_target_properties(spscring_bench_${bench} PROPERTIES FOLDER "Benchmarks")
  add_test(NAME spscring.bench.${bench} COMMAND spscring_bench_${bench})
  set_tests_properties(spscring.bench.${bench} PROPERTIES LABELS "spscring_benchmark" TIMEOUT 120)
endforeach()
