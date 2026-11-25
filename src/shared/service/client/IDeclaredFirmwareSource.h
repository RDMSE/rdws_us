// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <string>

namespace rdws::firmware {

// What a station last declared about its firmware, from its latest diagnostics snapshot
// (Plano_OTA.md §5). Everything but the build is absent on today's firmware.
struct DeclaredFirmware {
  std::string build;                   // fw_build
  std::string version;                 // fw_version
  std::optional<std::string> boardClass; // fw_class (O7)
  std::optional<std::string> rejected; // fw_rej: "<build>/<attempt>/<cause>" (O11, O18)
  std::optional<int> percent;          // ota_pct, while downloading
  std::string declaredAt;              // the snapshot's time, ISO 8601 UTC
};

// statusCode 200 = answered; `declared` is empty when the device never declared a build. Anything
// else is a failure to find out, kept apart so it's never read as "never declared".
struct DeclaredFirmwareLookup {
  int statusCode = 200;
  std::string errorMessage;
  std::optional<DeclaredFirmware> declared;
};

// Narrow interface for FirmwareService: device_telemetry belongs to device_service
// (device.diagnostics).
class IDeclaredFirmwareSource {
public:
  virtual ~IDeclaredFirmwareSource() = default;

  [[nodiscard]] virtual DeclaredFirmwareLookup declaredFirmware(const std::string& deviceId) = 0;
};

} // namespace rdws::firmware
