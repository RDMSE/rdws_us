#pragma once

#include "../../shared/service/IFieldValidator.h"

#include <memory>

namespace servicegateway {
class ServiceClient;
}

namespace rdws::field {

// Validates field_id FKs by calling field_service's "field.get" capability through
// the gateway (ServiceClient::invoke), instead of reaching into field_service's own
// repository/DB in-process. See backlog "chamada síncrona entre serviços via Gateway"
// in docs/Plano_Gateway_HTTP.md.
//
// Holds a reference to the owning service's ServiceClient pointer (not the object
// itself) because that pointer is replaced on broker reconnects.
class FieldServiceClient : public IFieldValidator {
public:
  explicit FieldServiceClient(const std::unique_ptr<servicegateway::ServiceClient>& client)
      : client_(client) {}

  [[nodiscard]] bool exists(const std::string& fieldId) override;

private:
  const std::unique_ptr<servicegateway::ServiceClient>& client_;
};

} // namespace rdws::field
