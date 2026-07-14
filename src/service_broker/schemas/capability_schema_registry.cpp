#include "capability_schema_registry.h"

#include "device_config_schemas.h"
#include "device_schemas.h"
#include "farm_schemas.h"
#include "field_schemas.h"
#include "sensor_schemas.h"

namespace servicegateway::schemas {

using rdws::utils::validator::SchemaValidator;

CapabilitySchemaRegistry::CapabilitySchemaRegistry() {
  auto add = [this](const std::string& capability, const std::string& schema) {
    validators_.emplace(capability, SchemaValidator::fromString(capability, schema));
  };

  add("farm.create", FARM_CREATE_SCHEMA);
  add("farm.update", FARM_UPDATE_SCHEMA);
  add("field.create", FIELD_CREATE_SCHEMA);
  add("field.update", FIELD_UPDATE_SCHEMA);
  add("device.create", DEVICE_CREATE_SCHEMA);
  add("device.update", DEVICE_UPDATE_SCHEMA);
  add("sensor.create", SENSOR_CREATE_SCHEMA);
  add("sensor.update", SENSOR_UPDATE_SCHEMA);
  add("device_config.update", DEVICE_CONFIG_UPDATE_SCHEMA);
}

std::vector<rdws::utils::validator::ValidationError>
CapabilitySchemaRegistry::validate(const std::string& capability,
                                   const rapidjson::Document& body) const {
  const auto it = validators_.find(capability);
  if (it == validators_.end()) {
    return {};
  }
  return it->second.validate(body);
}

} // namespace servicegateway::schemas
