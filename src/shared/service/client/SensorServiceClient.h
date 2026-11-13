// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ISensorValidator.h"

#include <memory>

namespace servicegateway {
class ServiceClient;
}

namespace rdws::sensor {

// Resolves sensor public ids by calling sensor_service's internal "sensor.ref" capability
// through the gateway (ServiceClient::invoke), instead of reaching into sensor_service's own
// repository/DB in-process. Mirrors rdws::field::FieldServiceClient.
//
// Holds a reference to the owning service's ServiceClient pointer (not the object
// itself) because that pointer is replaced on broker reconnects.
class SensorServiceClient : public ISensorValidator {
public:
  explicit SensorServiceClient(const std::unique_ptr<servicegateway::ServiceClient>& client)
      : client_(client) {}

  [[nodiscard]] SensorValidation resolve(const std::string& sensorPublicId) override;

private:
  const std::unique_ptr<servicegateway::ServiceClient>& client_;
};

} // namespace rdws::sensor
