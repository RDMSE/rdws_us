cmake_minimum_required(VERSION 3.16)

include(FetchContent)

# Zentraler Einstiegspunkt fuer Third-Party-Abhaengigkeiten.
add_library(inih_lib
	inih/ini.c
	inih/cpp/INIReader.cpp
	inih/cpp/INIReader.h
)
target_include_directories(inih_lib PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/inih
	${CMAKE_CURRENT_SOURCE_DIR}/inih/cpp
)

set(valijson_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(valijson_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(valijson EXCLUDE_FROM_ALL)

# Create wrapper library for ValiJSON with proper configuration
add_library(valijson_wrapper INTERFACE)
target_link_libraries(valijson_wrapper INTERFACE ValiJSON::valijson)
target_include_directories(valijson_wrapper INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/valijson/include
)

# dotenv-cpp (header-only library)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(dotenv-cpp EXCLUDE_FROM_ALL)

# Create wrapper library for dotenv-cpp with proper configuration
add_library(dotenv_wrapper INTERFACE)
target_link_libraries(dotenv_wrapper INTERFACE dotenv)
target_include_directories(dotenv_wrapper INTERFACE 
    ${CMAKE_CURRENT_SOURCE_DIR}/dotenv-cpp/include
)

add_library(rapidjson_wrapper INTERFACE)
target_include_directories(rapidjson_wrapper INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/valijson/thirdparty/rapidjson/include
)

# tl::expected (header-only library)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    tl_expected
    GIT_REPOSITORY https://github.com/TartanLlama/expected.git
    GIT_TAG v1.1.0
)
FetchContent_MakeAvailable(tl_expected)

add_library(expected_wrapper INTERFACE)
target_include_directories(expected_wrapper INTERFACE
    ${tl_expected_SOURCE_DIR}/include
)

# cpp-httplib (header-only HTTP server)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(httplib)

add_library(httplib_wrapper INTERFACE)
target_include_directories(httplib_wrapper INTERFACE
    ${httplib_SOURCE_DIR}
)

# fmt (string formatting — used directly and by spdlog)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.0.2
)
FetchContent_MakeAvailable(fmt)

# spdlog (structured logging) — use external fmt instead of bundled
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
)
FetchContent_MakeAvailable(spdlog)

add_library(spdlog_wrapper INTERFACE)
target_link_libraries(spdlog_wrapper INTERFACE fmt::fmt)
target_include_directories(spdlog_wrapper INTERFACE
    ${spdlog_SOURCE_DIR}/include
)
