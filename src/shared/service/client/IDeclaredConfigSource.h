// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <string>

namespace rdws::device_config {

// What a station last declared about its config, from its latest diagnostics snapshot
// (Plano_Telemetria.md D11): the version it runs and, while it refuses one, which.
struct DeclaredConfig {
  int declared = 0;            // cfg_version
  std::optional<int> rejected; // cfg_rej
  std::string declaredAt;      // the snapshot's time, ISO 8601 UTC
};

// statusCode 200 = answered; `declared` is empty when the device never declared a version
// (simulator, old firmware, never sent). Anything else is a failure to find out (device gone,
// gateway down, timeout), kept apart so it's never read as "never declared".
struct DeclaredLookup {
  int statusCode = 200;
  std::string errorMessage;
  std::optional<DeclaredConfig> declared;
};

// Narrow interface for DeviceConfigService: device_telemetry belongs to device_service
// (device.diagnostics), so the config side asks for it instead of reading the table.
class IDeclaredConfigSource {
public:
  virtual ~IDeclaredConfigSource() = default;

  [[nodiscard]] virtual DeclaredLookup declared(const std::string& deviceId) = 0;
};

} // namespace rdws::device_config
