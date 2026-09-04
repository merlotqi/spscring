# Sanitizer wiring for the spscring INTERFACE target.
#
#   spscring_enable_sanitizers(<target> <comma-separated-sanitizers>)
#
# Flags are added with INTERFACE scope on the INTERFACE library so that every
# consumer that links it (tests, benchmarks, examples) is instrumented. This is
# how the ASan/UBSan CI job (SPSCRING_SANITIZE=address,undefined) works.
#
# Compiler mapping:
#   - GCC / Clang : -fsanitize=<list> -fno-omit-frame-pointer
#   - MSVC        : /fsanitize=<list>

function(spscring_enable_sanitizers target sanitizers)
  if(NOT sanitizers)
    return()
  endif()

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${target} INTERFACE -fsanitize=${sanitizers} -fno-omit-frame-pointer)
    target_link_options(${target} INTERFACE -fsanitize=${sanitizers})
    target_compile_definitions(${target} INTERFACE SPSCRING_BUILD_SANITIZED=1)
  elseif(MSVC)
    target_compile_options(${target} INTERFACE /fsanitize=${sanitizers})
  endif()
endfunction()