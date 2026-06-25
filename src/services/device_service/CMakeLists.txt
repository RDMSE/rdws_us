add_executable(device_service device_service.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/repository/DeviceRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/DeviceService.cpp
)

target_link_libraries(device_service PRIVATE
    service_gateway_core
    rdws_shared_database
)

target_include_directories(device_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/service_broker
    ${CMAKE_SOURCE_DIR}/src/shared
)

set_target_properties(device_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
