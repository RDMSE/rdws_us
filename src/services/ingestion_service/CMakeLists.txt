add_executable(ingestion_service AppIngestionService.cpp)

target_link_libraries(ingestion_service PRIVATE
    service_gateway_core
    rdws_shared_config
    rdws_shared_crypto
    rdws_shared_coap
    rdws_shared_amqp
    rdws_shared_logger
)

target_include_directories(ingestion_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/service_broker
    ${CMAKE_SOURCE_DIR}/src/shared
)

set_target_properties(ingestion_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
