# Version of the solution (README, "Versões"). The git tag is the single source: vMAJOR.MINOR.PATCH
# on main (SemVer; v0.x until the API settles, v1.0.0 after Fase 13).
#
#   - -DRDWS_VERSION=... wins: the Docker build has no .git (.dockerignore), so CI computes
#     `git describe` and passes it as a build arg.
#   - Otherwise scripts/rdws_version.sh (`git describe` limited to SemVer tags), at configure
#     time: re-run cmake to refresh it after a commit or a new tag.
#
# Produces the INTERFACE target rdws_version with the generated header rdws_version.h.

set(RDWS_VERSION "" CACHE STRING "Version of the solution; empty = from git describe")

# Same rule as CI and the deploy: scripts/rdws_version.sh (RDWS_VERSION in its environment wins)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "RDWS_VERSION=${RDWS_VERSION}"
          bash "${PROJECT_SOURCE_DIR}/scripts/rdws_version.sh"
  OUTPUT_VARIABLE RDWS_VERSION_EFFECTIVE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _rdws_version_rc)
if(NOT _rdws_version_rc EQUAL 0 OR NOT RDWS_VERSION_EFFECTIVE)
  set(RDWS_VERSION_EFFECTIVE "v0.0.0-unknown")
endif()

message(STATUS "rdws version: ${RDWS_VERSION_EFFECTIVE}")

configure_file("${CMAKE_CURRENT_LIST_DIR}/rdws_version.h.in"
               "${PROJECT_BINARY_DIR}/generated/rdws_version.h" @ONLY)
add_library(rdws_version INTERFACE)
target_include_directories(rdws_version INTERFACE "${PROJECT_BINARY_DIR}/generated")
