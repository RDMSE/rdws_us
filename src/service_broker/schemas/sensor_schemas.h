#ifndef RDWS_US_SENSOR_SCHEMAS_H
#define RDWS_US_SENSOR_SCHEMAS_H

#include <string>

namespace servicegateway::schemas {

// JSON Schema for the `sensor.create` capability request body.
const std::string SENSOR_CREATE_SCHEMA = R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "title": "sensor.create",
        "properties": {
            "device_id": {
                "type": "string",
                "pattern": "^[0-9]+$"
            },
            "type": {
                "type": "string",
                "minLength": 1
            },
            "unit": {
                "type": "string",
                "minLength": 1
            }
        },
        "required": ["device_id", "type", "unit"]
    })";

// JSON Schema for the `sensor.update` capability request body.
const std::string SENSOR_UPDATE_SCHEMA = R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "title": "sensor.update",
        "properties": {
            "type": {
                "type": "string",
                "minLength": 1
            },
            "unit": {
                "type": "string",
                "minLength": 1
            }
        },
        "required": ["type", "unit"]
    })";

} // namespace servicegateway::schemas

#endif // RDWS_US_SENSOR_SCHEMAS_H
