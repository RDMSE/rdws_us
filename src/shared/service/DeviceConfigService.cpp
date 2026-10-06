#include "DeviceConfigService.h"

#include "../utils/json_merge.h"
#include "../validator/schema_validator.h"
#include "DeviceConfigSchemas.h"

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

} // namespace

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

  // execCommand only reflects failure in SQL/connection error — not in "0 rows affected" —
  // that's why the existence check above ensures the correct 404.
  if (!repo_.update(deviceId, {mergedJson})) {
    return OperationResult::error("Failed to update device config", 500);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Updated"});
}

} // namespace rdws::device_config
