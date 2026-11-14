# Example Service CMakeLists.txt

# Create the example service executable
add_executable(example_service example_service.cpp)

# Link with rdws_broker_client for ServiceClient access
target_link_libraries(example_service PRIVATE rdws_broker_client)

# Include directories for ServiceGateway headers
target_include_directories(example_service PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/service_broker
    ${CMAKE_SOURCE_DIR}/src/shared
)

# Set output directory to bin
set_target_properties(example_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Copy to build directory for easy testing
add_custom_command(TARGET example_service POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    $<TARGET_FILE:example_service>
    ${CMAKE_BINARY_DIR}/bin/example_service
    COMMENT "Copying example_service to bin directory"
)