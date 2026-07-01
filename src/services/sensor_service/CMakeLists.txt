add_executable(sensor_service AppSensorService.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/repository/SensorRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/SensorService.cpp
)

target_link_libraries(sensor_service PRIVATE
    service_gateway_core
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
