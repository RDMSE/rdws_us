add_executable(sensor_simulator_service AppSensorSimulatorService.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/repository/DeviceRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/repository/DeviceConfigRepository.cpp
)

target_link_libraries(sensor_simulator_service PRIVATE
    service_gateway_core
    rdws_shared_database
    rdws_shared_config
    rdws_shared_crypto
    rdws_shared_coap
    rdws_shared_logger
    httplib_wrapper
)

target_include_directories(sensor_simulator_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/service_broker
    ${CMAKE_SOURCE_DIR}/src/shared
)

set_target_properties(sensor_simulator_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
