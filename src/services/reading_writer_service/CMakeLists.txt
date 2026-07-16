add_executable(reading_writer_service AppReadingWriterService.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/repository/SensorReadingRepository.cpp
)

target_link_libraries(reading_writer_service PRIVATE
    rdws_shared_database
    rdws_shared_config
    rdws_shared_amqp
    rdws_shared_logger
    rdws_shared_utils
    rdws_shared_json_utils
)

target_include_directories(reading_writer_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/shared
)

set_target_properties(reading_writer_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
