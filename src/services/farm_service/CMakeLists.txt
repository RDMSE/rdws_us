add_executable(farm_service AppFarmService.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/repository/FarmRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/service/FarmService.cpp
)

target_link_libraries(farm_service PRIVATE
    service_gateway_core
    rdws_shared_database
)

target_include_directories(farm_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/service_broker
    ${CMAKE_SOURCE_DIR}/src/shared
)

set_target_properties(farm_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
