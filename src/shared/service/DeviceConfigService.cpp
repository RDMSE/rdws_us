#include "DeviceConfigService.h"

#include "../utils/json_merge.h"
#include "../validator/schema_validator.h"
#include "DeviceConfigSchemas.h"

#include <rapidjson/document.h>

#include <unordered_map>

namespace rdws::device_config {

using rdws::types::OperationResult;
using rdws::types::OperationStatus;
namespace json = rdws::utils::json;

namespace {

// "" when valid. Only real weather stations have a schema: simulated devices carry the
// SensorSimulatorService's own keys (sampling_interval_s, ...), and other types have no
// firmware depending on a shape yet.
std::string validateConfig(const DeviceConfig& device, const std::string& mergedJson) {
  if (device.deviceType != "weather_station" || device.isSimulated) {
    return {};
  }
  static const auto validator = rdws::utils::validator::SchemaValidator::fromString(
      "weather_station config", WEATHER_STATION_CONFIG_SCHEMA);
  std::string errors;
  for (const auto& e : validator.validate(mergedJson)) {
    errors += (errors.empty() ? "" : "; ") + e.field + ": " + e.message;
  }
  return errors;
}

// sensors.type each onboard channel feeds
const std::unordered_map<std::string, std::string> kChannelSensorType = {
    {"temp", "temperature"},
    {"humidity", "humidity"},
    {"press", "pressure"},
};

} // namespace

// "" when every onboard[].sensor_id is a sensor of this device of the channel's type.
// Without it, a swapped id is accepted and the station's readings are then dropped by the
// IngestionService (the SenML unit doesn't convert to the sensor's unit) — silently, from
// the operator's point of view. Runs after the schema check, so the shape is known.
std::string DeviceConfigService::checkSensors(const std::string& deviceId,
                                              const std::string& mergedJson) {
  rapidjson::Document doc;
  doc.Parse(mergedJson.c_str());
  const auto onboard = doc.FindMember("onboard");
  if (onboard == doc.MemberEnd() || !onboard->value.IsArray()) {
    return {};
  }

  std::unordered_map<std::string, std::string> typeById;
  for (const auto& s : sensors_.findAll(deviceId)) {
    typeById.emplace(s.id, s.type);
  }

  std::string errors;
  rapidjson::SizeType i = 0;
  for (const auto& entry : onboard->value.GetArray()) {
    const std::string chan = entry["chan"].GetString();
    const std::string sensorId = std::to_string(entry["sensor_id"].GetUint64());
    const std::string where = "onboard[" + std::to_string(i++) + "]: ";
    const auto sensor = typeById.find(sensorId);
    if (sensor == typeById.end()) {
      errors += (errors.empty() ? "" : "; ") + where + "sensor " + sensorId +
                " is not a sensor of device " + deviceId;
    } else if (sensor->second != kChannelSensorType.at(chan)) {
      errors += (errors.empty() ? "" : "; ") + where + "sensor " + sensorId + " is " +
                sensor->second + ", chan " + chan + " needs " + kChannelSensorType.at(chan);
    }
  }
  return errors;
}

std::optional<DeviceConfig> DeviceConfigService::findByDeviceId(const std::string& deviceId) {
  return repo_.findByDeviceId(deviceId);
}

OperationResult DeviceConfigService::update(const std::string& deviceId,
                                            const DeviceConfigUpdate& data) {
  const auto existing = repo_.findByDeviceId(deviceId);
  if (!existing) {
    return OperationResult::error("Device config not found for device_id " + deviceId, 404);
  }

  const std::string mergedJson = json::mergePatch(existing->config, data.configJson);
  if (const auto errors = validateConfig(*existing, mergedJson); !errors.empty()) {
    return OperationResult::error("Invalid weather_station config: " + errors, 400);
  }
  if (existing->deviceType == "weather_station" && !existing->isSimulated) {
    if (const auto errors = checkSensors(deviceId, mergedJson); !errors.empty()) {
      return OperationResult::error("Invalid weather_station config: " + errors, 400);
    }
  }

  // execCommand only reflects failure in SQL/connection error — not in "0 rows affected" —
  // that's why the existence check above ensures the correct 404.
  if (!repo_.update(deviceId, {mergedJson, data.updatedBy})) {
    return OperationResult::error("Failed to update device config", 500);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Updated"});
}

} // namespace rdws::device_config
