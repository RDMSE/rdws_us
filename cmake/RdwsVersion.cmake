# Version of the solution (README, "Versões"). The git tag is the single source: vMAJOR.MINOR.PATCH
# on main (SemVer; v0.x until the API settles, v1.0.0 after Fase 13).
#
#   - -DRDWS_VERSION=... wins: the Docker build has no .git (.dockerignore), so CI computes
#     `git describe` and passes it as a build arg.
#   - Otherwise `git describe --tags --match "v[0-9]*" --dirty`, at configure time (re-run
#     cmake to refresh it). Only SemVer tags count: older tags like development/phase-5/end
#     are ignored.
#   - No SemVer tag yet, or no git: v0.0.0-dev+<short sha> / v0.0.0-unknown.
#
# Produces the INTERFACE target rdws_version with the generated header rdws_version.h.

set(RDWS_VERSION "" CACHE STRING "Version of the solution; empty = from git describe")

if(NOT RDWS_VERSION)
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*" --dirty --always
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      OUTPUT_VARIABLE _rdws_describe
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _rdws_describe_rc)
  endif()
  if(_rdws_describe_rc EQUAL 0 AND _rdws_describe MATCHES "^v[0-9]")
    set(RDWS_VERSION_EFFECTIVE "${_rdws_describe}")
  elseif(_rdws_describe_rc EQUAL 0 AND _rdws_describe)
    # --always without a matching tag: just the commit
    set(RDWS_VERSION_EFFECTIVE "v0.0.0-dev+g${_rdws_describe}")
  else()
    set(RDWS_VERSION_EFFECTIVE "v0.0.0-unknown")
  endif()
else()
  set(RDWS_VERSION_EFFECTIVE "${RDWS_VERSION}")
endif()

message(STATUS "rdws version: ${RDWS_VERSION_EFFECTIVE}")

configure_file("${CMAKE_CURRENT_LIST_DIR}/rdws_version.h.in"
               "${PROJECT_BINARY_DIR}/generated/rdws_version.h" @ONLY)
add_library(rdws_version INTERFACE)
target_include_directories(rdws_version INTERFACE "${PROJECT_BINARY_DIR}/generated")
