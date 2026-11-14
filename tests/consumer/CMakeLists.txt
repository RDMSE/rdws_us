# SPDX-FileCopyrightText: 2026 Rafael Dias
# SPDX-License-Identifier: Apache-2.0
#
# Smoke test of rdws_us as a dependency (README, "Licença"; open-core split): a project
# outside this repository includes it and links only the shared libraries, as the private
# repository will (FetchContent or a submodule at a tag). Run by ci.yml:
#   cmake -S tests/consumer -B build-consumer && cmake --build build-consumer && build-consumer/hello
cmake_minimum_required(VERSION 3.16)
project(rdws_consumer_smoke LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

# RDWS_SHARED_ONLY is ON by default for an including project; said here so the test reads
set(RDWS_SHARED_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. rdws_us)

add_executable(hello hello.cpp)
target_link_libraries(hello PRIVATE
    rdws_broker_client
    rdws_shared_logger
)

# Neither the gateway nor any service may be part of this build
foreach(_target service_gateway_core service_gateway_http farm_service)
  if(TARGET ${_target})
    message(FATAL_ERROR "RDWS_SHARED_ONLY built ${_target}")
  endif()
endforeach()
