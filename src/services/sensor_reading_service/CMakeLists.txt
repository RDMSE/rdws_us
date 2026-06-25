add_executable(sensor_reading_service sensor_reading_service.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/repository/SensorReadingRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/SensorReadingService.cpp
)

target_link_libraries(sensor_reading_service PRIVATE
    service_gateway_core
    rdws_shared_database
)

target_include_directories(sensor_reading_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/service_broker
    ${CMAKE_SOURCE_DIR}/src/shared
)

set_target_properties(sensor_reading_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
