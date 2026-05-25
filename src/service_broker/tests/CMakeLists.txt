add_executable(service_gateway_test 
        test_service_schema.cpp
)
target_link_libraries(service_gateway_test PRIVATE service_gateway_core)

if(TARGET GTest::gtest_main)
    target_link_libraries(service_gateway_test PRIVATE GTest::gtest_main)
elseif(TARGET gtest_main)
    target_link_libraries(service_gateway_test PRIVATE gtest_main)
else()
    message(FATAL_ERROR "GoogleTest target not found. Check FetchContent/find_package setup.")
endif()

include(GoogleTest)
gtest_discover_tests(service_gateway_test)

# --- ServiceGateway metrics / health endpoint tests ---
add_executable(service_gateway_metrics_test
        test_gateway_metrics.cpp
)
target_link_libraries(service_gateway_metrics_test PRIVATE
    service_gateway_core
    gtest_main
    gtest
)
gtest_discover_tests(service_gateway_metrics_test)

# --- ServiceGateway integration tests (gateway + ServiceClient full cycle) ---
add_executable(service_gateway_integration_test
        test_gateway_integration.cpp
)
target_link_libraries(service_gateway_integration_test PRIVATE
    service_gateway_core
    gtest_main
    gtest
)
gtest_discover_tests(service_gateway_integration_test
    PROPERTIES TIMEOUT 30  # each test has network I/O — allow 30s
)

# --- EventRouter unit tests ---
add_executable(service_gateway_router_test
        test_event_router.cpp
)
target_link_libraries(service_gateway_router_test PRIVATE
    service_gateway_core
    gtest_main
    gtest
)
gtest_discover_tests(service_gateway_router_test)
