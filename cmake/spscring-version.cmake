# Central version authority for spscring.
#
# Included by the top-level CMakeLists.txt; the single source of truth for
# project(), the installed CMake package version, and the pkg-config file.
# Bump these when cutting a release and tag it in git.

set(SPSCRING_VERSION_MAJOR 0)
set(SPSCRING_VERSION_MINOR 1)
set(SPSCRING_VERSION_PATCH 0)

set(SPSCRING_VERSION
    "${SPSCRING_VERSION_MAJOR}.${SPSCRING_VERSION_MINOR}.${SPSCRING_VERSION_PATCH}")