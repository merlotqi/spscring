# Compiler warning flags for first-party targets.
#
#   spscring_enable_warnings(<target>)
#
# Adds a common, portable warning set to a target. Warnings are deliberately
# NOT promoted to errors here (that is a repo-policy decision, not a good
# default for a consumable library) so the helper is safe to call on every
# first-party target without breaking a downstream build.

function(spscring_enable_warnings target)
  if(MSVC)
    # /utf-8: all first-party sources and bundled headers are UTF-8 (without BOM);
    # MSVC would otherwise assume the active code page (C4819) and could mangle
    # non-ASCII characters. MSVC 19.16+/VS2017 15.7+.
    target_compile_options(${target} PRIVATE /W4 /utf-8)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()