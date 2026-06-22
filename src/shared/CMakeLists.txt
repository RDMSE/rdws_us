cmake_minimum_required(VERSION 3.16)

# --- Shared Validation Library ---
add_library(rdws_shared_validation
    validator/schema_validator.cpp
    validator/schema_validator.h
)

target_include_directories(rdws_shared_validation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/validator
)

target_link_libraries(rdws_shared_validation PUBLIC
    valijson_wrapper
    rapidjson_wrapper
)

# jsoncpp is provided via valijson_wrapper (bundled)


add_library(rdws_shared_config
    config/config.cpp
    config/config.h
)

target_include_directories(rdws_shared_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(rdws_shared_config PUBLIC
    dotenv_wrapper
)

add_library(rdws_shared_types
    types/lambda_event.cpp
    types/lambda_event.h
    types/lambda_context.cpp
    types/lambda_context.h
    types/service_result.h
)

target_include_directories(rdws_shared_types PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/types
)

target_link_libraries(rdws_shared_types PUBLIC
    rapidjson_wrapper
)

add_library(rdws_shared_utils
    utils/lambda_params_helper.cpp
    utils/lambda_params_helper.h
    utils/metrics.cpp
    utils/metrics.h
    utils/response_helper.cpp
    utils/response_helper.h
)

target_include_directories(rdws_shared_utils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

target_link_libraries(rdws_shared_utils PUBLIC
    expected_wrapper
    rapidjson_wrapper
)

add_library(rdws_shared_logger
    utils/logger.cpp
    utils/logger.h
)

target_include_directories(rdws_shared_logger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

target_link_libraries(rdws_shared_logger PUBLIC
    spdlog_wrapper
)

add_library(rdws_shared_database
    database/postgresql_database.cpp
    database/postgresql_database.h
    database/idatabase.h
)

target_include_directories(rdws_shared_database PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/database
    ${PQXX_INCLUDE_DIRS}
)

# target_link_libraries(rdws_shared_database PUBLIC
#     rdws_shared_config
#     ${PQXX_LIBRARIES}
# )

target_link_libraries(rdws_shared_database PUBLIC
    rdws_shared_config
    libpqxx::pqxx
)

# --- Testes para shared components ---
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
