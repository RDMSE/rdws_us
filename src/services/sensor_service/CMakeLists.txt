add_executable(sensor_service AppSensorService.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/repository/SensorRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/SensorService.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/client/DeviceServiceClient.cpp
)

target_link_libraries(sensor_service PRIVATE
    rdws_broker_client
    rdws_shared_config
    rdws_shared_database
)

target_include_directories(sensor_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/service_broker
    ${CMAKE_SOURCE_DIR}/src/shared
)

set_target_properties(sensor_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
