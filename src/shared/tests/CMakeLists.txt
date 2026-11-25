cmake_minimum_required(VERSION 3.16)

# --- Schema validator tests ---
add_executable(shared_validation_tests
    test_schema_validator.cpp
)
target_link_libraries(shared_validation_tests PRIVATE
    rdws_shared_validation
    gtest_main
    gtest
)
include(GoogleTest)
gtest_discover_tests(shared_validation_tests)

# --- MetricsTracker unit tests ---
add_executable(shared_metrics_tests
    test_metrics.cpp
)
target_link_libraries(shared_metrics_tests PRIVATE
    rdws_shared_utils
    gtest_main
    gtest
)
gtest_discover_tests(shared_metrics_tests)

# --- JSON merge patch unit tests ---
add_executable(shared_json_merge_tests
    test_json_merge.cpp
)
target_link_libraries(shared_json_merge_tests PRIVATE
    rdws_shared_json_utils
    gtest_main
    gtest
)
gtest_discover_tests(shared_json_merge_tests)

# --- Logger unit tests ---
add_executable(shared_logger_tests
    test_logger.cpp
)
target_link_libraries(shared_logger_tests PRIVATE
    rdws_shared_logger
    gtest_main
    gtest
)
gtest_discover_tests(shared_logger_tests)

# --- CredentialCipher (AES-256-GCM) unit tests ---
add_executable(shared_credential_cipher_tests
    test_credential_cipher.cpp
)
target_link_libraries(shared_credential_cipher_tests PRIVATE
    rdws_shared_crypto
    gtest_main
    gtest
)
gtest_discover_tests(shared_credential_cipher_tests)

# --- MCUboot image parser unit tests ---
add_executable(shared_mcuboot_image_tests
    test_mcuboot_image.cpp
)
target_link_libraries(shared_mcuboot_image_tests PRIVATE
    rdws_shared_firmware
    gtest_main
    gtest
)
gtest_discover_tests(shared_mcuboot_image_tests)

add_executable(shared_file_image_store_tests
    test_file_image_store.cpp
)
target_include_directories(shared_file_image_store_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/shared
)
target_link_libraries(shared_file_image_store_tests PRIVATE
    rdws_shared_firmware
    gtest_main
    gtest
)
gtest_discover_tests(shared_file_image_store_tests)

add_executable(shared_authorization_tests
    test_authorization.cpp
)
target_link_libraries(shared_authorization_tests PRIVATE
    rdws_shared_utils
    gtest_main
    gtest
)
gtest_discover_tests(shared_authorization_tests)

add_executable(shared_firmware_service_tests
    test_firmware_service.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/FirmwareService.cpp
)
target_include_directories(shared_firmware_service_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/shared
)
target_link_libraries(shared_firmware_service_tests PRIVATE
    rdws_shared_firmware
    rdws_shared_types
    rdws_shared_database
    gtest_main
    gtest
)
gtest_discover_tests(shared_firmware_service_tests)

# --- CoapDtlsClient (libcoap PSK/DTLS) integration test — real handshake against a
# local in-process libcoap PSK server, see test file header comment. ---
add_executable(shared_coap_dtls_client_tests
    test_coap_dtls_client.cpp
)
target_link_libraries(shared_coap_dtls_client_tests PRIVATE
    rdws_shared_coap
    gtest_main
    gtest
)
gtest_discover_tests(shared_coap_dtls_client_tests)

# --- Shared ID validation tests ---
add_executable(shared_id_validation_tests
    test_validation.cpp
)
target_link_libraries(shared_id_validation_tests PRIVATE
    rdws_shared_utils
    gtest_main
    gtest
)
gtest_discover_tests(shared_id_validation_tests)
# --- json_helper actor identity unit tests ---
add_executable(shared_json_helper_tests
    test_json_helper.cpp
)
target_link_libraries(shared_json_helper_tests PRIVATE
    rdws_shared_json_utils
    gtest_main
    gtest
)
gtest_discover_tests(shared_json_helper_tests)

# --- SenML parser unit tests ---
add_executable(shared_senml_tests
    test_senml.cpp
)
target_link_libraries(shared_senml_tests PRIVATE
    rdws_shared_senml
    gtest_main
    gtest
)
gtest_discover_tests(shared_senml_tests)

# --- DeviceConfigService unit tests (weather_station config schema) ---
add_executable(shared_device_config_service_tests
    test_device_config_service.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/DeviceConfigService.cpp
)
target_include_directories(shared_device_config_service_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/shared
)
target_link_libraries(shared_device_config_service_tests PRIVATE
    rdws_shared_validation
    rdws_shared_json_utils
    rdws_shared_types
    rdws_shared_database
    gtest_main
    gtest
)
gtest_discover_tests(shared_device_config_service_tests)

add_executable(shared_farm_service_tests
    test_farm_service.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/FarmService.cpp
)
target_include_directories(shared_farm_service_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/shared
)
target_link_libraries(shared_farm_service_tests PRIVATE
    rdws_shared_types
    rdws_shared_database
    gtest_main
    gtest
)
gtest_discover_tests(shared_farm_service_tests)

add_executable(shared_field_service_tests
    test_field_service.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/FieldService.cpp
)
target_include_directories(shared_field_service_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/shared
)
target_link_libraries(shared_field_service_tests PRIVATE
    rdws_shared_types
    rdws_shared_database
    gtest_main
    gtest
)
gtest_discover_tests(shared_field_service_tests)

add_executable(shared_device_service_tests
    test_device_service.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/DeviceService.cpp
)
target_include_directories(shared_device_service_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/shared
)
target_link_libraries(shared_device_service_tests PRIVATE
    rdws_shared_types
    rdws_shared_database
    gtest_main
    gtest
)
gtest_discover_tests(shared_device_service_tests)

add_executable(shared_sensor_service_tests
    test_sensor_service.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/SensorService.cpp
)
target_include_directories(shared_sensor_service_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/shared
)
target_link_libraries(shared_sensor_service_tests PRIVATE
    rdws_shared_types
    rdws_shared_database
    gtest_main
    gtest
)
gtest_discover_tests(shared_sensor_service_tests)

add_executable(shared_sensor_reading_service_tests
    test_sensor_reading_service.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/SensorReadingService.cpp
)
target_include_directories(shared_sensor_reading_service_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/shared
)
target_link_libraries(shared_sensor_reading_service_tests PRIVATE
    rdws_shared_types
    rdws_shared_database
    gtest_main
    gtest
)
gtest_discover_tests(shared_sensor_reading_service_tests)

add_executable(shared_refresh_schedule_tests
    test_refresh_schedule.cpp
)
target_link_libraries(shared_refresh_schedule_tests PRIVATE
    rdws_shared_utils
    gtest_main
    gtest
)
gtest_discover_tests(shared_refresh_schedule_tests)

add_executable(shared_version_tests
    test_version.cpp
)
target_link_libraries(shared_version_tests PRIVATE
    rdws_version
    gtest_main
    gtest
)
gtest_discover_tests(shared_version_tests)

add_executable(shared_service_client_heartbeat_tests
    test_service_client_heartbeat.cpp
)
target_link_libraries(shared_service_client_heartbeat_tests PRIVATE
    rdws_broker_client
    gtest_main
    gtest
)
gtest_discover_tests(shared_service_client_heartbeat_tests
    PROPERTIES TIMEOUT 30 # fake broker over a UNIX socket, a few seconds each
)
