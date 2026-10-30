// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: AGPL-3.0-only

#ifndef RDWS_US_CAPABILITY_SCHEMA_REGISTRY_H
#define RDWS_US_CAPABILITY_SCHEMA_REGISTRY_H

#include "../../shared/validator/schema_validator.h"

#include <rapidjson/document.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace servicegateway::schemas {

// Maps write capability names (e.g. "farm.create") to the SchemaValidator that
// validates their request body. Capabilities with no registered schema (reads,
// deletes, or anything not yet covered) are simply not validated here.
class CapabilitySchemaRegistry {
public:
  CapabilitySchemaRegistry();

  [[nodiscard]] std::vector<rdws::utils::validator::ValidationError>
  validate(const std::string& capability, const rapidjson::Document& body) const;

private:
  std::unordered_map<std::string, rdws::utils::validator::SchemaValidator> validators_;
};

} // namespace servicegateway::schemas

#endif // RDWS_US_CAPABILITY_SCHEMA_REGISTRY_H
