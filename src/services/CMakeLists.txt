# Services CMakeLists.txt
# This file manages all services in the project

add_subdirectory(example_service)
add_subdirectory(auth_service)
add_subdirectory(farm_service)
add_subdirectory(field_service)
add_subdirectory(device_service)
add_subdirectory(device_config_service)
add_subdirectory(sensor_service)
add_subdirectory(sensor_reading_service)
add_subdirectory(persistence_service)
# sensor_simulator_service builds here (same CMake project) but is intentionally left
# out of the copy_services_binaries loop below — it's a standalone tool, not part of
# the main compose (Plano_Deployment.md §1, Plano_SensorSimulatorService.md).
add_subdirectory(sensor_simulator_service)

# Create services output directory
set(SERVICES_OUTPUT_DIR ${CMAKE_BINARY_DIR}/services)
file(MAKE_DIRECTORY ${SERVICES_OUTPUT_DIR})

# Copy all service binaries to a common location
add_custom_target(copy_services_binaries ALL
    COMMENT "Copying service binaries to services directory"
)

foreach(SVC
    example_service
    auth_service
    farm_service
    field_service
    device_service
    device_config_service
    sensor_service
    sensor_reading_service
    persistence_service
)
    add_custom_command(TARGET copy_services_binaries POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${SVC}>
            ${SERVICES_OUTPUT_DIR}/${SVC}
        COMMENT "Copying ${SVC} to services directory"
    )
    add_dependencies(copy_services_binaries ${SVC})
endforeach()
