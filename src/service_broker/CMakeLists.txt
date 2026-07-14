
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
        Services/EventBus.h
        Services/EventBus.cpp
        # Auth middleware
        Auth/AuthMiddleware.h
        Auth/AuthMiddleware.cpp
        # Config
        Config/GatewayConfig.h
        Config/GatewayConfig.cpp
        # Utils
        Utils/Utils.cpp
        Utils/Utils.h
        # Request schema validation (capability -> SchemaValidator)
        schemas/capability_schema_registry.h
        schemas/capability_schema_registry.cpp
        schemas/farm_schemas.h
        schemas/field_schemas.h
        schemas/device_schemas.h
        schemas/sensor_schemas.h
        schemas/device_config_schemas.h
)

# Compatibility alias — old name kept for external consumers.
add_library(service_broker_core ALIAS service_gateway_core)

find_package(OpenSSL REQUIRED)

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
    rdws_shared_json_utils
    OpenSSL::Crypto
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

# --- Unit tests ---
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

