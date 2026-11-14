add_executable(device_config_service AppDeviceConfigService.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/repository/DeviceConfigRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/repository/SensorRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/DeviceConfigService.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/client/DeviceDiagnosticsClient.cpp
)

target_link_libraries(device_config_service PRIVATE
    rdws_broker_client
    rdws_shared_config
    rdws_shared_database
    rdws_shared_validation
)

target_include_directories(device_config_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/service_broker
    ${CMAKE_SOURCE_DIR}/src/shared
)

set_target_properties(device_config_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
