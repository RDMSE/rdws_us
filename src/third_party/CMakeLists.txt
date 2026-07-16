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

# Third-party deps below use URL (tarball) downloads instead of GIT_REPOSITORY —
# a plain HTTPS GET has a bounded TIMEOUT/INACTIVITY_TIMEOUT and fails fast on a
# flaky/blocked connection, whereas `git clone` (no timeout by default) can hang the
# whole CI job indefinitely on a network hiccup (same reasoning as googletest above).

# tl::expected (header-only library)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    tl_expected
    URL https://github.com/TartanLlama/expected/archive/refs/tags/v1.1.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TIMEOUT 60
    INACTIVITY_TIMEOUT 30
)
FetchContent_MakeAvailable(tl_expected)

add_library(expected_wrapper INTERFACE)
target_include_directories(expected_wrapper INTERFACE
    ${tl_expected_SOURCE_DIR}/include
)

# cpp-httplib (header-only HTTP server)
FetchContent_Declare(
    httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.15.3.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TIMEOUT 60
    INACTIVITY_TIMEOUT 30
)
FetchContent_MakeAvailable(httplib)

add_library(httplib_wrapper INTERFACE)
target_include_directories(httplib_wrapper INTERFACE
    ${httplib_SOURCE_DIR}
)

# fmt (string formatting — used directly and by spdlog)
FetchContent_Declare(
    fmt
    URL https://github.com/fmtlib/fmt/archive/refs/tags/11.0.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TIMEOUT 60
    INACTIVITY_TIMEOUT 30
)
FetchContent_MakeAvailable(fmt)

# spdlog (structured logging) — use external fmt instead of bundled
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TIMEOUT 60
    INACTIVITY_TIMEOUT 30
)
FetchContent_MakeAvailable(spdlog)

add_library(spdlog_wrapper INTERFACE)
target_link_libraries(spdlog_wrapper INTERFACE fmt::fmt)
target_include_directories(spdlog_wrapper INTERFACE
    ${spdlog_SOURCE_DIR}/include
)

# libcoap (CoAP protocol, CoapDtlsClient in src/shared/coap) — built with the OpenSSL
# DTLS backend, reusing the OpenSSL::* targets already required by the root
# CMakeLists.txt instead of pulling in GnuTLS/TinyDTLS/MbedTLS as a second crypto stack.
set(ENABLE_DTLS ON CACHE BOOL "" FORCE)
set(DTLS_BACKEND "openssl" CACHE STRING "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libcoap
    URL https://github.com/obgm/libcoap/archive/refs/tags/v4.3.5.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TIMEOUT 60
    INACTIVITY_TIMEOUT 30
)
FetchContent_MakeAvailable(libcoap)

# rabbitmq-c (AMQP 0-9-1 client, used by the amqp_client wrapper in src/shared/amqp for
# IngestionService/ReadingWriterService) — static build, no new apt package needed
# (OpenSSL already required by the root CMakeLists.txt).
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_API_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_SSL_SUPPORT ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    rabbitmq_c
    URL https://github.com/alanxz/rabbitmq-c/archive/refs/tags/v0.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TIMEOUT 60
    INACTIVITY_TIMEOUT 30
)
FetchContent_MakeAvailable(rabbitmq_c)
