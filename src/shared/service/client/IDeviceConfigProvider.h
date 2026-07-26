#pragma once

#include <string>

namespace rdws::device_config {

// Result of fetching a device's current config via device_config.get. When found is
// false, statusCode carries the cause so callers can distinguish "no config for this
// device" (404) from transient/operational failures (gateway disconnected, invoke
// timeout/502), mirroring rdws::farm::FarmValidation.
struct DeviceConfigResult {
  bool found = false;
  int statusCode = 404;
  std::string configJson; // raw JSON of the "config" field, only set when found
  std::string errorMessage;
};

// Narrow interface used by services that only need to *read* a device's current
// config (e.g. IngestionService piggybacking it on a CoAP ACK), without pulling in
// the rest of DeviceConfigService's CRUD surface.
class IDeviceConfigProvider {
public:
  virtual ~IDeviceConfigProvider() = default;

  [[nodiscard]] virtual DeviceConfigResult getConfig(const std::string& deviceId) = 0;
};

} // namespace rdws::device_config
