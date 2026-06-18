add_executable(field_service field_service.cpp)

target_link_libraries(field_service PRIVATE
    service_gateway_core
    rdws_shared_database
)

target_include_directories(field_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/service_broker
    ${CMAKE_SOURCE_DIR}/src/shared
)

set_target_properties(field_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
