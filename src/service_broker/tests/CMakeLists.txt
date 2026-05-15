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
