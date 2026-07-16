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