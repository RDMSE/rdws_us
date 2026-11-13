// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "DeviceConfigService.h"

#include "../utils/json_helper.h"
#include "../utils/json_merge.h"
#include "../validator/schema_validator.h"
#include "DeviceConfigSchemas.h"

#include <rapidjson/document.h>

#include <unordered_map>
#include <unordered_set>

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
constexpr int kDefaultAggWindowS = 600;

const std::unordered_map<std::string, std::string> kChannelSensorType = {
    {"temp", "temperature"},
    {"humidity", "humidity"},
    {"press", "pressure"},
};

} // namespace

// "" when every trigger fits the rest of the config. The firmware rejects a config with a
// rule outside these limits as a whole and keeps the previous one, with nothing telling the
// operator, so they're refused here first (Plano_Firmware_WeatherNode.md §4, "Validação").
// Runs on the merged config: changing agg_window_s can invalidate a win_s stored earlier.
std::string DeviceConfigService::checkTriggers(const std::string& mergedJson) {
  rapidjson::Document doc;
  doc.Parse(mergedJson.c_str());
  const auto triggers = doc.FindMember("triggers");
  if (triggers == doc.MemberEnd() || !triggers->value.IsArray()) {
    return {};
  }

  // Channels the station samples. Unit conversion also goes through onboard[] (chan →
  // sensor_id → sensors.unit), so a rule on the build's default channels can't be delivered.
  std::unordered_set<std::string> sampled;
  if (const auto onboard = doc.FindMember("onboard");
      onboard != doc.MemberEnd() && onboard->value.IsArray()) {
    for (const auto& entry : onboard->value.GetArray()) {
      if (json::getBool(entry, "enabled").value_or(true)) {
        sampled.insert(entry["chan"].GetString());
      }
    }
  }

  // Without the key the window is the firmware's build default: 600 s in production, 60 s in
  // bench.conf. A multiple of 600 fits both.
  const int window = json::getInt(doc, "agg_window_s").value_or(kDefaultAggWindowS);

  std::string errors;
  rapidjson::SizeType i = 0;
  for (const auto& rule : triggers->value.GetArray()) {
    const std::string chan = rule["chan"].GetString();
    const std::string where = "triggers[" + std::to_string(i++) + "]: ";
    if (!sampled.contains(chan)) {
      errors += (errors.empty() ? "" : "; ") + where + "chan " + chan +
                " is not an enabled channel in onboard[]";
    }
    if (const auto winS = json::getInt(rule, "win_s"); winS && *winS % window != 0) {
      errors += (errors.empty() ? "" : "; ") + where + "win_s " + std::to_string(*winS) +
                " is not a multiple of agg_window_s (" + std::to_string(window) + ")";
    }
  }
  return errors;
}

// "" when every onboard[].sensor_id is a sensor of this device of the channel's type.
// Without it, a swapped id is accepted and the station's readings are then dropped by the
// IngestionService (the SenML unit doesn't convert to the sensor's unit) — silently, from
// the operator's point of view. Runs after the schema check, so the shape is known.
std::string DeviceConfigService::checkSensors(const std::string& devicePublicId,
                                              const std::string& mergedJson) {
  rapidjson::Document doc;
  doc.Parse(mergedJson.c_str());
  const auto onboard = doc.FindMember("onboard");
  if (onboard == doc.MemberEnd() || !onboard->value.IsArray()) {
    return {};
  }

  std::unordered_map<std::string, std::string> typeById;
  for (const auto& s : sensors_.findAll(devicePublicId)) {
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
                " is not a sensor of device " + devicePublicId;
    } else if (sensor->second != kChannelSensorType.at(chan)) {
      errors += (errors.empty() ? "" : "; ") + where + "sensor " + sensorId + " is " +
                sensor->second + ", chan " + chan + " needs " + kChannelSensorType.at(chan);
    }
  }
  return errors;
}

std::string configState(int version, std::optional<int> declared, std::optional<int> rejected) {
  if (!declared) {
    return "unknown";
  }
  if (*declared == version) {
    return "applied";
  }
  if (rejected && *rejected == version) {
    return "rejected";
  }
  return "pending";
}

rdws::types::ServiceResult<ConfigStatus>
DeviceConfigService::status(const std::string& devicePublicId) {
  using Result = rdws::types::ServiceResult<ConfigStatus>;
  const auto config = repo_.findByDevicePublicId(devicePublicId);
  if (!config) {
    return Result::error("Configuration not found", 404);
  }
  const auto lookup = declaredSource_.declared(devicePublicId); // device.diagnostics, by UUID
  if (lookup.statusCode != 200) {
    return Result::error(lookup.errorMessage, lookup.statusCode);
  }
  ConfigStatus s;
  s.version = config->version;
  if (lookup.declared) {
    s.declared = lookup.declared->declared;
    s.rejected = lookup.declared->rejected;
    s.declaredAt = lookup.declared->declaredAt;
  }
  s.state = configState(s.version, s.declared, s.rejected);
  return Result::success(s);
}

std::optional<DeviceConfig>
DeviceConfigService::findByDevicePublicId(const std::string& devicePublicId) {
  return repo_.findByDevicePublicId(devicePublicId);
}

OperationResult DeviceConfigService::update(const std::string& devicePublicId,
                                            const DeviceConfigUpdate& data) {
  const auto existing = repo_.findByDevicePublicId(devicePublicId);
  if (!existing) {
    return OperationResult::error("Device config not found for device_id " + devicePublicId,
                                  404);
  }

  const std::string mergedJson = json::mergePatch(existing->config, data.configJson);
  if (const auto errors = validateConfig(*existing, mergedJson); !errors.empty()) {
    return OperationResult::error("Invalid weather_station config: " + errors, 400);
  }
  if (existing->deviceType == "weather_station" && !existing->isSimulated) {
    // The device's sensors, listed by its public id; their ids stay internal (station protocol)
    if (const auto errors = checkSensors(existing->devicePublicId, mergedJson); !errors.empty()) {
      return OperationResult::error("Invalid weather_station config: " + errors, 400);
    }
    if (const auto errors = checkTriggers(mergedJson); !errors.empty()) {
      return OperationResult::error("Invalid weather_station config: " + errors, 400);
    }
  }

  // execCommand only reflects failure in SQL/connection error — not in "0 rows affected" —
  // that's why the existence check above ensures the correct 404.
  if (!repo_.update(existing->deviceId, {mergedJson, data.updatedBy})) {
    return OperationResult::error("Failed to update device config", 500);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Updated"});
}

} // namespace rdws::device_config
