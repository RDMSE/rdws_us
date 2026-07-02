#include "schema_validator.h"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

using namespace rdws::utils::validator;

class SchemaValidatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Simple schema for testing
    testSchema = R"({
            "type": "object",
            "properties": {
                "name": {"type": "string"},
                "age": {"type": "integer", "minimum": 0}
            },
            "required": ["name"]
        })";
  }

  std::string testSchema;
};

TEST_F(SchemaValidatorTest, ValidatesCorrectJson) {
  auto validator = SchemaValidator::fromString("test", testSchema);

  rapidjson::Document validJson;
  validJson.SetObject();
  auto& allocator = validJson.GetAllocator();
  validJson.AddMember("name", "John Doe", allocator);
  validJson.AddMember("age", 30, allocator);

  EXPECT_TRUE(validator.isValid(validJson));
  EXPECT_TRUE(validator.validate(validJson).empty());
}

TEST_F(SchemaValidatorTest, DetectsMissingRequiredField) {
  auto validator = SchemaValidator::fromString("test", testSchema);

  rapidjson::Document invalidJson;
  invalidJson.SetObject();
  invalidJson.AddMember("age", 25, invalidJson.GetAllocator()); // missing required "name" field

  EXPECT_FALSE(validator.isValid(invalidJson));

  auto errors = validator.validate(invalidJson);
  EXPECT_FALSE(errors.empty());
  EXPECT_EQ(errors.size(), 1);
  // valijson reports missing required fields at root level
  EXPECT_EQ(errors[0].field, "<root>");
  EXPECT_NE(errors[0].message.find("name"), std::string::npos);
}

TEST_F(SchemaValidatorTest, DetectsWrongType) {
  auto validator = SchemaValidator::fromString("test", testSchema);

  rapidjson::Document invalidJson;
  invalidJson.SetObject();
  auto& allocator = invalidJson.GetAllocator();
  invalidJson.AddMember("name", "Jane", allocator);
  invalidJson.AddMember("age", "thirty", allocator); // should be integer

  EXPECT_FALSE(validator.isValid(invalidJson));

  auto errors = validator.validate(invalidJson);
  EXPECT_FALSE(errors.empty());
}

TEST_F(SchemaValidatorTest, ValidatesJsonString) {
  auto validator = SchemaValidator::fromString("test", testSchema);

  std::string validJsonString = R"({"name": "Alice", "age": 25})";
  std::string invalidJsonString = R"({"age": 25})"; // missing name

  EXPECT_TRUE(validator.validate(validJsonString).empty());
  EXPECT_FALSE(validator.validate(invalidJsonString).empty());
}
