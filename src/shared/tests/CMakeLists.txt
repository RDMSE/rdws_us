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