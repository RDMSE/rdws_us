#ifndef RDWS_US_DEVICE_SCHEMAS_H
#define RDWS_US_DEVICE_SCHEMAS_H

#include <string>

namespace servicegateway::schemas {

// JSON Schema for the `device.create` capability request body.
// installation_date's precise ISO-8601 format is still validated in
// AppDeviceService.cpp — this schema only enforces basic shape.
const std::string DEVICE_CREATE_SCHEMA = R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "title": "device.create",
        "properties": {
            "field_id": {
                "type": "string",
                "pattern": "^[0-9]+$"
            },
            "type": {
                "type": "string",
                "minLength": 1
            },
            "status": {
                "type": "string"
            },
            "installation_date": {
                "type": "string"
            }
        },
        "required": ["field_id", "type"]
    })";

// JSON Schema for the `device.update` capability request body.
const std::string DEVICE_UPDATE_SCHEMA = R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "title": "device.update",
        "properties": {
            "type": {
                "type": "string",
                "minLength": 1
            },
            "status": {
                "type": "string",
                "minLength": 1
            }
        },
        "required": ["type", "status"]
    })";

} // namespace servicegateway::schemas

#endif // RDWS_US_DEVICE_SCHEMAS_H
