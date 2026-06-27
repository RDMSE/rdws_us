#pragma once

#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <utility>
#include <valijson/schema.hpp>
#include <valijson/validator.hpp>
#include <vector>

namespace rdws::validation {

struct ValidationError {
  std::string field;
  std::string message;
  std::string context;

  ValidationError(std::string f, std::string m, std::string c = "")
      : field(std::move(f)), message(std::move(m)), context(std::move(c)) {}
};

class SchemaValidator {
  std::string schemaName;
  std::unique_ptr<valijson::Schema> schema;
  std::unique_ptr<valijson::Validator> validator;

  // Private default constructor for factory methods
  SchemaValidator() = default;

  [[nodiscard]] static std::string getSchemaPath(const std::string& schemaFile);
  [[nodiscard]] bool loadSchemaFromString(const std::string& schemaString) const;
  [[nodiscard]] static std::vector<ValidationError>
  convertValidationResults(const valijson::ValidationResults& results);

public:
  // Constructor for string-based schemas (recommended)
  static SchemaValidator fromString(const std::string& name, const std::string& schemaString);

  ~SchemaValidator() = default;

  // Move constructor and assignment
  SchemaValidator(SchemaValidator&& other) noexcept;
  SchemaValidator& operator=(SchemaValidator&& other) noexcept;

  // Disable copy
  SchemaValidator(const SchemaValidator&) = delete;
  SchemaValidator& operator=(const SchemaValidator&) = delete;

  [[nodiscard]] std::vector<ValidationError> validate(const rapidjson::Document& json) const;
  [[nodiscard]] std::vector<ValidationError> validate(const std::string& jsonString) const;

  [[nodiscard]] bool isValid(const rapidjson::Document& json) const {
    return validate(json).empty();
  }

  [[nodiscard]] bool isValid(const std::string& jsonString) const {
    return validate(jsonString).empty();
  }

  [[nodiscard]] std::string getErrorsAsJson(const std::vector<ValidationError>& errors) const;

  [[nodiscard]] const std::string& getName() const {
    return schemaName;
  }
};

}; // namespace rdws::validation
