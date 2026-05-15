#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <memory>
#include "validator/schema_validator.h"
#include "schemas/service.h"

using namespace rdws::validation;
using namespace servicegateway::schemas;

class ServiceSchemaTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create validators for both schemas using factory methods
        serviceValidator = std::make_unique<SchemaValidator>(
            SchemaValidator::fromString("service", SERVICE_SCHEMA)
        );
        servicesArrayValidator = std::make_unique<SchemaValidator>(
            SchemaValidator::fromString("services_array", SERVICES_ARRAY_SCHEMA)
        );
    }

    std::unique_ptr<SchemaValidator> serviceValidator;
    std::unique_ptr<SchemaValidator> servicesArrayValidator;
};

TEST_F(ServiceSchemaTest, ValidatesSingleServiceObject) {
    rapidjson::Document validService;
    validService.SetObject();
    auto &alloc = validService.GetAllocator();
    validService.AddMember("name", "test-service", alloc);
    validService.AddMember("path", "./services/test", alloc);
    validService.AddMember("instances", 2, alloc);

    EXPECT_TRUE(serviceValidator->isValid(validService));
    EXPECT_TRUE(serviceValidator->validate(validService).empty());
}

TEST_F(ServiceSchemaTest, RejectsServiceWithMissingName) {
    rapidjson::Document invalidService;
    invalidService.SetObject();
    auto &alloc = invalidService.GetAllocator();
    invalidService.AddMember("path", "./services/test", alloc);
    invalidService.AddMember("instances", 1, alloc);

    EXPECT_FALSE(serviceValidator->isValid(invalidService));

    auto errors = serviceValidator->validate(invalidService);
    EXPECT_FALSE(errors.empty());
}

TEST_F(ServiceSchemaTest, RejectsServiceWithInvalidInstancesCount) {
    rapidjson::Document invalidService;
    invalidService.SetObject();
    auto &alloc = invalidService.GetAllocator();
    invalidService.AddMember("name", "test-service", alloc);
    invalidService.AddMember("path", "./services/test", alloc);
    invalidService.AddMember("instances", -1, alloc); // Invalid: negative instances

    EXPECT_FALSE(serviceValidator->isValid(invalidService));

    auto errors = serviceValidator->validate(invalidService);
    EXPECT_FALSE(errors.empty());
}

TEST_F(ServiceSchemaTest, ValidatesServicesArray) {
    rapidjson::Document servicesArray;
    servicesArray.SetArray();

    rapidjson::Value service1(rapidjson::kObjectType);
    service1.AddMember("name", "service-test1", servicesArray.GetAllocator());
    service1.AddMember("path", "./services/test1", servicesArray.GetAllocator());
    service1.AddMember("instances", 1, servicesArray.GetAllocator());

    rapidjson::Value service2(rapidjson::kObjectType);
    service2.AddMember("name", "service-test2", servicesArray.GetAllocator());
    service2.AddMember("path", "./services/test2", servicesArray.GetAllocator());
    service2.AddMember("instances", 1, servicesArray.GetAllocator());

    servicesArray.PushBack(service1, servicesArray.GetAllocator());
    servicesArray.PushBack(service2, servicesArray.GetAllocator());

    EXPECT_TRUE(servicesArrayValidator->isValid(servicesArray));
    EXPECT_TRUE(servicesArrayValidator->validate(servicesArray).empty());
}

TEST_F(ServiceSchemaTest, ValidatesExistingServicesJsonStructure) {
    // Test with the actual structure from services.json
    std::string servicesJsonContent = R"([
        {
            "name": "service-test1",
            "path": "./services/test1",
            "instances": 1
        },
        {
            "name": "service-test2",
            "path": "./services/test2",
            "instances": 1
        }
    ])";

    auto errors = servicesArrayValidator->validate(servicesJsonContent);
    EXPECT_TRUE(errors.empty()) << "Existing services.json should be valid";
}
