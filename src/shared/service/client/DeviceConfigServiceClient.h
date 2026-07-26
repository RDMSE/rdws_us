#pragma once

#include "IDeviceConfigProvider.h"

#include <memory>

namespace servicegateway {
class ServiceClient;
}

namespace rdws::device_config {

// Calls device_config_service's "device_config.get" capability through the gateway
// (ServiceClient::invoke), instead of a service reaching into device_config's own
// repository/DB in-process. Mirrors rdws::field::FieldServiceClient /
// rdws::farm::FarmServiceClient.
//
// Holds a reference to the owning service's ServiceClient pointer (not the object
// itself) because that pointer is replaced on broker reconnects.
class DeviceConfigServiceClient : public IDeviceConfigProvider {
public:
  explicit DeviceConfigServiceClient(const std::unique_ptr<servicegateway::ServiceClient>& client)
      : client_(client) {}

  [[nodiscard]] DeviceConfigResult getConfig(const std::string& deviceId) override;

private:
  const std::unique_ptr<servicegateway::ServiceClient>& client_;
};

} // namespace rdws::device_config
