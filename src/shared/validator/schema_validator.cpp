
#include "schema_validator.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <utility>
#include <valijson/schema_parser.hpp>
#include <valijson/validation_results.hpp>

#include "../../shared/utils/json_helper.h"

namespace json = rdws::utils::json;

namespace rdws::utils::validator {

// Static factory method for string-based schemas
SchemaValidator SchemaValidator::fromString(const std::string& name,
                                            const std::string& schemaString) {
  SchemaValidator validator;
  validator.schemaName = name;
  validator.schema = std::make_unique<valijson::Schema>();
  validator.validator = std::make_unique<valijson::Validator>();

  if (!validator.loadSchemaFromString(schemaString)) {
    throw std::runtime_error("Failed to parse schema: " + name);
  }

  return validator;
}

SchemaValidator::SchemaValidator(SchemaValidator&& other) noexcept
    : schemaName(std::move(other.schemaName)), schema(std::move(other.schema)),
      validator(std::move(other.validator)) {}

SchemaValidator& SchemaValidator::operator=(SchemaValidator&& other) noexcept {
  if (this != &other) {
    schemaName = std::move(other.schemaName);
    schema = std::move(other.schema);
    validator = std::move(other.validator);
  }
  return *this;
}

std::string SchemaValidator::getSchemaPath(const std::string& schemaFile) {
  // Try relative to current directory first (new location in src)
  std::string relativePath = "src/schemas/" + schemaFile;
  if (std::filesystem::exists(relativePath)) {
    return relativePath;
  }

  // Try old schemas path for backward compatibility
  if (std::string oldPath = "schemas/" + schemaFile; std::filesystem::exists(oldPath)) {
    return oldPath;
  }

  // Try absolute path
  if (std::filesystem::exists(schemaFile)) {
    return schemaFile;
  }

  // Default to new schemas directory
  return relativePath;
}

bool SchemaValidator::loadSchemaFromString(const std::string& schemaString) const {
  try {
    rapidjson::Document schemaDoc;
    schemaDoc.Parse(schemaString.c_str());
    if (schemaDoc.HasParseError()) {
      std::cerr << "Schema parse error at offset " << schemaDoc.GetErrorOffset() << '\n';
      return false;
    }

    // Create schema from parsed JSON
    valijson::SchemaParser parser;
    const valijson::adapters::RapidJsonAdapter schemaAdapter(schemaDoc);
    parser.populateSchema(schemaAdapter, *schema);

    return true;
  } catch (const std::exception& e) {
    std::cerr << "Error parsing schema string: " << e.what() << '\n';
    return false;
  }
}

std::vector<ValidationError> SchemaValidator::validate(const rapidjson::Document& json) const {
  valijson::ValidationResults results;

  if (valijson::adapters::RapidJsonAdapter targetAdapter(json);
      validator->validate(*schema, targetAdapter, &results)) {
    return {}; // No errors
  }

  return convertValidationResults(results);
}

std::vector<ValidationError> SchemaValidator::validate(const std::string& jsonString) const {
  rapidjson::Document json;
  json.Parse(jsonString.c_str());
  if (json.HasParseError()) {
    std::ostringstream oss;
    oss << "Invalid JSON format at offset " << json.GetErrorOffset();
    return {ValidationError("root", oss.str())};
  }

  return validate(json);
}

std::vector<ValidationError>
SchemaValidator::convertValidationResults(const valijson::ValidationResults& results) {
  std::vector<ValidationError> errors;

  // Note: results.popError modifies the object, so we need non-const
  auto& mutableResults = const_cast<valijson::ValidationResults&>(results);

  valijson::ValidationResults::Error error;
  while (mutableResults.popError(error)) {
    std::string fieldPath;
    for (const auto& token : error.context) {
      if (!fieldPath.empty()) {
        fieldPath += ".";
      }
      fieldPath += token;
    }

    if (fieldPath.empty()) {
      fieldPath = "<root>";
    }

    errors.emplace_back(fieldPath, error.description, "");
  }

  return errors;
}

std::string SchemaValidator::getErrorsAsJson(const std::vector<ValidationError>& errors) const {
  rapidjson::Document result;
  result.SetObject();
  auto& allocator = result.GetAllocator();

  rapidjson::Value errorsArray(rapidjson::kArrayType);
  for (const auto& error : errors) {
    json::JsonObj errorObj(allocator);
    errorObj.set("field", error.field);
    errorObj.set("message", error.message);

    if (!error.context.empty()) {
      errorObj.set("context", error.context);
    }

    errorsArray.PushBack(errorObj.take(), allocator);
  }

  rapidjson::Value resultValue = json::JsonObj(allocator)
      .set("valid", false)
      .set("schema", schemaName)
      .setValue("errors", std::move(errorsArray))
      .take();
   resultValue.Swap(result);

  return rdws::utils::json::docToString(result);
}

}; // namespace rdws::utils::validator
