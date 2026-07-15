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
    rdws_shared_json_utils
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

add_library(rdws_shared_json_utils
    utils/json_helper.cpp
    utils/json_helper.h
    utils/json_merge.cpp
    utils/json_merge.h
)

target_include_directories(rdws_shared_json_utils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

target_link_libraries(rdws_shared_json_utils PUBLIC
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

add_library(rdws_shared_crypto
    crypto/credential_cipher.cpp
    crypto/credential_cipher.h
)

target_include_directories(rdws_shared_crypto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto
)

target_link_libraries(rdws_shared_crypto PUBLIC
    OpenSSL::Crypto
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

target_link_libraries(rdws_shared_database
    PRIVATE
    PQXX_DEP
    pq
)

# Falhar cedo se usarmos API deprecated do libpqxx (ex.: exec_prepared em favor de
# exec(prepped, params) em versões mais novas) — evita divergência silenciosa entre
# a versão do libpqxx local e a do container CI/CD.
target_compile_options(rdws_shared_database PRIVATE -Werror=deprecated-declarations)

# Linux Mint Debian Edition
# target_link_libraries(rdws_shared_database PUBLIC
#     rdws_shared_config
#     libpqxx::pqxx
# )

# Linux Mint
# target_link_libraries(rdws_shared_database PUBLIC
#     rdws_shared_config
#     ${PQXX_LIBRARIES}
# )

# --- Testes para shared components ---
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
