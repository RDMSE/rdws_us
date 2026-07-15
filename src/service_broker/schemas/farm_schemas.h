#ifndef RDWS_US_FARM_SCHEMAS_H
#define RDWS_US_FARM_SCHEMAS_H

#include <string>

namespace servicegateway::schemas {

// JSON Schema for the `farm.create` capability request body.
const std::string FARM_CREATE_SCHEMA = R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "title": "farm.create",
        "properties": {
            "name": {
                "type": "string",
                "minLength": 1
            },
            "location": {
                "type": "object",
                "properties": {
                    "lat": { "type": "number" },
                    "lng": { "type": "number" }
                }
            }
        },
        "required": ["name"]
    })";

// JSON Schema for the `farm.update` capability request body.
const std::string FARM_UPDATE_SCHEMA = R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "title": "farm.update",
        "properties": {
            "name": {
                "type": "string",
                "minLength": 1
            }
        },
        "required": ["name"]
    })";

} // namespace servicegateway::schemas

#endif // RDWS_US_FARM_SCHEMAS_H
