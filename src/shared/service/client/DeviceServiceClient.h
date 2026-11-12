// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "IDeviceValidator.h"

#include <memory>

namespace servicegateway {
class ServiceClient;
}

namespace rdws::device {

// Resolves device public ids by calling device_service's internal "device.ref" capability
// through the gateway (ServiceClient::invoke), instead of reaching into device_service's own
// repository/DB in-process. Mirrors rdws::field::FieldServiceClient.
//
// Holds a reference to the owning service's ServiceClient pointer (not the object
// itself) because that pointer is replaced on broker reconnects.
class DeviceServiceClient : public IDeviceValidator {
public:
  explicit DeviceServiceClient(const std::unique_ptr<servicegateway::ServiceClient>& client)
      : client_(client) {}

  [[nodiscard]] DeviceValidation resolve(const std::string& devicePublicId) override;

private:
  const std::unique_ptr<servicegateway::ServiceClient>& client_;
};

} // namespace rdws::device
