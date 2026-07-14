#ifndef RDWS_US_FIELD_SCHEMAS_H
#define RDWS_US_FIELD_SCHEMAS_H

#include <string>

namespace servicegateway::schemas {

// JSON Schema for the `field.create` capability request body.
const std::string FIELD_CREATE_SCHEMA = R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "title": "field.create",
        "properties": {
            "farm_id": {
                "type": "string",
                "pattern": "^[0-9]+$"
            },
            "name": {
                "type": "string",
                "minLength": 1
            },
            "area": {
                "type": ["number", "string"]
            }
        },
        "required": ["farm_id", "name"]
    })";

// JSON Schema for the `field.update` capability request body.
const std::string FIELD_UPDATE_SCHEMA = R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "title": "field.update",
        "properties": {
            "name": {
                "type": "string",
                "minLength": 1
            }
        },
        "required": ["name"]
    })";

} // namespace servicegateway::schemas

#endif // RDWS_US_FIELD_SCHEMAS_H
