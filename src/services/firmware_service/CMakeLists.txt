add_executable(firmware_service AppFirmwareService.cpp
    FirmwareCoapServer.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/repository/FirmwareRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/FirmwareService.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/client/DeviceDiagnosticsClient.cpp
)

target_link_libraries(firmware_service PRIVATE
    rdws_broker_client
    rdws_shared_config
    rdws_shared_database
    rdws_shared_firmware
    rdws_shared_coap
)

target_include_directories(firmware_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/service_broker
    ${CMAKE_SOURCE_DIR}/src/shared
)

set_target_properties(firmware_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
