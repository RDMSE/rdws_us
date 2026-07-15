cmake_minimum_required(VERSION 3.16)

project(rdws_us VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

include(CTest)
if(BUILD_TESTING)
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
# NOTE: src/third_party forces BUILD_TESTING OFF (to suppress third-party tests).
# Restore it so our own test subdirectories are included.
add_subdirectory(src/third_party)
set(BUILD_TESTING ON CACHE BOOL "" FORCE)

# --- Shared Components ---
add_subdirectory(src/shared)

# --- Subprojetos ---
add_subdirectory(src/service_broker)

# --- Services ---
add_subdirectory(src/services)

# --- Copy global configuration files ---
add_custom_target(copy_env_file ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/.env.dev
        ${CMAKE_BINARY_DIR}/.env
    COMMENT "Copying .env.dev to build directory as .env"
)
