// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "IDeclaredConfigSource.h"
#include "IDeclaredFirmwareSource.h"

#include <memory>
#include <optional>
#include <rapidjson/document.h>
#include <string>

namespace servicegateway {
class ServiceClient;
}

namespace rdws::device {

// Reads what a station declared about its config or its firmware by calling device_service's
// "device.diagnostics" capability through the gateway (ServiceClient::invoke), instead of
// reading device_telemetry from another service. Mirrors rdws::farm::FarmServiceClient.
//
// Holds a reference to the owning service's ServiceClient pointer (not the object itself)
// because that pointer is replaced on broker reconnects.
class DeviceDiagnosticsClient : public rdws::device_config::IDeclaredConfigSource,
                                public rdws::firmware::IDeclaredFirmwareSource {
public:
  explicit DeviceDiagnosticsClient(const std::unique_ptr<servicegateway::ServiceClient>& client)
      : client_(client) {}

  [[nodiscard]] rdws::device_config::DeclaredLookup declared(const std::string& deviceId) override;
  [[nodiscard]] rdws::firmware::DeclaredFirmwareLookup
  declaredFirmware(const std::string& deviceId) override;

private:
  // The capability's answer: a failure, or the snapshot ({"timestamp", "data"}; data is null when
  // the device never sent diagnostics)
  struct Snapshot {
    int statusCode = 200;
    std::string errorMessage;
    rapidjson::Document envelope;
    const rapidjson::Value* body = nullptr; // inside envelope
    const rapidjson::Value* data = nullptr;
  };
  [[nodiscard]] std::unique_ptr<Snapshot> snapshot(const std::string& deviceId);

  const std::unique_ptr<servicegateway::ServiceClient>& client_;
};

} // namespace rdws::device
