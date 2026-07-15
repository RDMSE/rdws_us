#ifndef RDWS_US_DEVICE_CONFIG_SCHEMAS_H
#define RDWS_US_DEVICE_CONFIG_SCHEMAS_H

#include <string>

namespace servicegateway::schemas {

// JSON Schema for the `device_config.update` capability request body.
// `config` is stored/serialized as-is by DeviceConfigService — this schema only
// requires its presence, not any particular internal shape.
const std::string DEVICE_CONFIG_UPDATE_SCHEMA = R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "title": "device_config.update",
        "properties": {
            "config": {}
        },
        "required": ["config"]
    })";

} // namespace servicegateway::schemas

#endif // RDWS_US_DEVICE_CONFIG_SCHEMAS_H
