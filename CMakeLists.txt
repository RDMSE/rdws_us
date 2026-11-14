cmake_minimum_required(VERSION 3.16)

project(rdws_us VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/RdwsVersion.cmake)

# Included by another project (the private repo pulls rdws_us with FetchContent or as a
# submodule): only what that project needs, and nothing that changes its own settings.
# PROJECT_IS_TOP_LEVEL needs CMake 3.21; this is the same test.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(RDWS_TOP_LEVEL ON)
  set(_rdws_shared_only_default OFF)
else()
  set(RDWS_TOP_LEVEL OFF)
  set(_rdws_shared_only_default ON)
endif()

# Only the shared libraries (src/shared, Apache-2.0, the broker client included) and what they
# need (src/third_party): no gateway, services or tests. Off in this repository's own build,
# on by default when another project includes it.
option(RDWS_SHARED_ONLY "Build only the shared libraries (src/shared)" ${_rdws_shared_only_default})

# Our tests only in our own full build: a project that includes this one keeps its BUILD_TESTING
set(RDWS_BUILD_TESTS OFF)

option(ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)

if(ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy)
  if(CLANG_TIDY_EXE)
    set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE})
    message(STATUS "clang-tidy enabled: ${CLANG_TIDY_EXE}")
  else()
    message(WARNING "ENABLE_CLANG_TIDY is ON, but clang-tidy was not found")
  endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS ON)

# Workaround for RapidJSON v1.1.0 with GCC 14 const member assignment issue
# Treat it as warning instead of error
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=narrowing -Wno-error=class-memaccess")
add_compile_options(-fpermissive)

# Configure debug settings
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
    message(STATUS "Debug mode enabled with symbols")
endif()

if(RDWS_TOP_LEVEL AND NOT RDWS_SHARED_ONLY)
  include(CTest)
  set(RDWS_BUILD_TESTS ${BUILD_TESTING})
endif()
if(RDWS_BUILD_TESTS)
  include(FetchContent)

  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TIMEOUT 60
    INACTIVITY_TIMEOUT 30
  )
  FetchContent_MakeAvailable(googletest)
endif()

if(libpqxx_FOUND)
    message(STATUS "libpqxx found via CMake config")
    add_library(PQXX_DEP ALIAS libpqxx::pqxx)
else()
    message(STATUS "libpqxx CMake config not found, using pkg-config")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
    add_library(PQXX_DEP ALIAS PkgConfig::PQXX)
endif()

# OpenSSL — used by service_broker (JWT HMAC) and src/shared/crypto (AES-256-GCM for
# device credentials). find_package here (root) instead of only in service_broker's
# CMakeLists.txt so src/shared and src/third_party (libcoap's DTLS backend) can also
# link OpenSSL::Crypto/OpenSSL::SSL regardless of add_subdirectory order.
find_package(OpenSSL REQUIRED)

# Linux Mint
# find_package(libpqxx REQUIRED)

# Linux Mint Debian Edition
# find_package(PkgConfig REQUIRED)
# pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)

# --- Third-party ---
# NOTE: src/third_party forces BUILD_TESTING OFF in the cache (to suppress third-party tests).
# Put back the value it had, so neither our test subdirectories nor an including project's
# BUILD_TESTING are affected.
if(DEFINED CACHE{BUILD_TESTING})
  set(_rdws_build_testing_before "$CACHE{BUILD_TESTING}")
endif()
add_subdirectory(src/third_party)
if(DEFINED _rdws_build_testing_before)
  set(BUILD_TESTING "${_rdws_build_testing_before}" CACHE BOOL "" FORCE)
else()
  unset(BUILD_TESTING CACHE)
endif()
# The subdirectories test BUILD_TESTING; inside this project it means our own tests
set(BUILD_TESTING ${RDWS_BUILD_TESTS})

# --- Shared Components ---
add_subdirectory(src/shared)

if(NOT RDWS_SHARED_ONLY)
  # --- Subprojetos ---
  add_subdirectory(src/service_broker)

  # --- Services ---
  add_subdirectory(src/services)
endif()

# --- Copy global configuration files ---
if(RDWS_TOP_LEVEL AND NOT RDWS_SHARED_ONLY)
  add_custom_target(copy_env_file ALL
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${PROJECT_SOURCE_DIR}/.env.dev
          ${PROJECT_BINARY_DIR}/.env
      COMMENT "Copying .env.dev to build directory as .env"
  )
endif()
