add_executable(auth_service auth_service.cpp)

target_link_libraries(auth_service PRIVATE
    service_gateway_core
    rdws_shared_database
)

target_include_directories(auth_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/service_broker
    ${CMAKE_SOURCE_DIR}/src/shared
)

set_target_properties(auth_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
