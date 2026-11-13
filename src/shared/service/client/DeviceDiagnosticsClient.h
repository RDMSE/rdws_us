// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "IDeclaredConfigSource.h"

#include <memory>

namespace servicegateway {
class ServiceClient;
}

namespace rdws::device {

// Reads what a station declared about its config by calling device_service's
// "device.diagnostics" capability through the gateway (ServiceClient::invoke), instead of
// reading device_telemetry from the config service. Mirrors rdws::farm::FarmServiceClient.
//
// Holds a reference to the owning service's ServiceClient pointer (not the object itself)
// because that pointer is replaced on broker reconnects.
class DeviceDiagnosticsClient : public rdws::device_config::IDeclaredConfigSource {
public:
  explicit DeviceDiagnosticsClient(const std::unique_ptr<servicegateway::ServiceClient>& client)
      : client_(client) {}

  [[nodiscard]] rdws::device_config::DeclaredLookup declared(const std::string& deviceId) override;

private:
  const std::unique_ptr<servicegateway::ServiceClient>& client_;
};

} // namespace rdws::device
