#pragma once

#include "IFarmValidator.h"

#include <memory>

namespace servicegateway {
class ServiceClient;
}

namespace rdws::farm {

// Validates farm_id FKs by calling farm_service's "farm.get" capability through
// the gateway (ServiceClient::invoke), instead of reaching into farm_service's own
// repository/DB in-process. Mirrors rdws::field::FieldServiceClient.
//
// Holds a reference to the owning service's ServiceClient pointer (not the object
// itself) because that pointer is replaced on broker reconnects.
class FarmServiceClient : public IFarmValidator {
public:
  explicit FarmServiceClient(const std::unique_ptr<servicegateway::ServiceClient>& client)
      : client_(client) {}

  [[nodiscard]] FarmValidation exists(const std::string& farmId) override;

private:
  const std::unique_ptr<servicegateway::ServiceClient>& client_;
};

} // namespace rdws::farm
