#ifndef RDWS_US_SERVICE_SCHEMA_H
#define RDWS_US_SERVICE_SCHEMA_H

#include <string>

namespace servicegateway::schemas {

// Function to generate the service properties schema (DRY approach)
inline std::string getServicePropertiesJson() {
  return R"(
            "properties": {
                "active": {
                    "type": "boolean",
                    "description": "Indicates whether the service is active and should be loaded",
                    "default": false
                },
                "name": {
                    "type": "string",
                    "description": "The name identifier of the service",
                    "minLength": 1,
                    "maxLength": 100,
                    "pattern": "^[a-zA-Z0-9_-]+$"
                },
                "path": {
                    "type": "string",
                    "description": "The filesystem path to the service executable or directory",
                    "minLength": 1,
                    "maxLength": 500
                },
                "instances": {
                    "type": "integer",
                    "description": "Number of service instances to run",
                    "minimum": 0,
                    "maximum": 100,
                    "default": 1
                }
            },
            "required": ["name", "path", "instances"],
            "additionalProperties": false
        )";
}

// JSON Schema for validating individual service objects
// Corresponds to the 'service' struct in ../service.h
const std::string SERVICE_SCHEMA = R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "title": "Service Definition",
        "description": "Schema for validating a single service configuration",)" +
                                   getServicePropertiesJson() + R"(})";

// JSON Schema for validating arrays of services (like services.json)
const std::string SERVICES_ARRAY_SCHEMA = R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "array",
        "title": "Services Collection",
        "description": "Schema for validating an array of service configurations",
        "items": {
            "type": "object",
            "title": "Service Definition",
            "description": "Schema for validating a single service configuration",)" +
                                          getServicePropertiesJson() +
                                          R"(},
        "minItems": 0,
        "maxItems": 50
    })";

} // namespace servicegateway::schemas

#endif // RDWS_US_SERVICE_SCHEMA_H
