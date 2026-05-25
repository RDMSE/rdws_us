
add_library(service_gateway_core
        # ServiceGateway Architecture
        Services/ServiceIdentity.h
        Services/ServiceIdentity.cpp
        Services/ServiceRegistry.h
        Services/ServiceRegistry.cpp
        Services/ServiceGateway.h
        Services/ServiceGateway.cpp
        Services/ServiceClient.h
        Services/ServiceClient.cpp
        Services/ServiceMonitor.h
        Services/ServiceMonitor.cpp
        Services/EventRouter.h
        Services/EventRouter.cpp
        # Utils
        Utils/Utils.cpp
        Utils/Utils.h
)

# Compatibility alias — old name kept for external consumers.
add_library(service_broker_core ALIAS service_gateway_core)

target_include_directories(service_gateway_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas
    ../third_party
    ../shared
)

target_link_libraries(service_gateway_core PUBLIC
    inih_lib
    ValiJSON::valijson
    rdws_shared_validation
    rdws_shared_config
    rdws_shared_logger
    rdws_shared_utils
)

add_executable(service_gateway_http
    service_gateway_http.cpp
    HttpGateway.cpp
)

# Compatibility alias — old name kept for external consumers.
add_executable(service_broker_http ALIAS service_gateway_http)

target_link_libraries(service_gateway_http PRIVATE
    service_gateway_core
    rdws_shared_types
    rdws_shared_logger
    httplib_wrapper
)

# --- Executables ---
add_executable(service_gateway_app
    main.cpp
)
target_link_libraries(service_gateway_app PRIVATE service_gateway_core)
add_executable(service_broker_app ALIAS service_gateway_app)

# ServiceGateway Examples
add_executable(service_gateway_example
    service_gateway_example.cpp
)
target_link_libraries(service_gateway_example PRIVATE service_gateway_core)
add_executable(service_broker_example ALIAS service_gateway_example)

add_executable(service_gateway_monitor
    service_gateway_monitor.cpp
)
target_link_libraries(service_gateway_monitor PRIVATE service_gateway_core)
add_executable(service_broker_monitor ALIAS service_gateway_monitor)

add_executable(example_service_client
    example_service_client.cpp
)
target_link_libraries(example_service_client PRIVATE service_gateway_core)

# --- Unit tests ---
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

