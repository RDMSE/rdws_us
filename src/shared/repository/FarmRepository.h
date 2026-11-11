// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>
#include <vector>

namespace rdws::farm {

struct Farm {
  std::string id;       // internal (BIGSERIAL): FK target, never sent to API clients
  std::string publicId; // UUID: the id the API exposes and receives
  std::string name;
  std::string location; // WKT or empty
  std::string timezone; // IANA name (V17), local day of the derived indices
  std::string createdAt;
  std::string updatedAt;
  std::string updatedBy;
};

struct FarmCreate {
  std::string name;
  std::string locationWkt; // empty = no geometry
  std::string timezone;    // empty = the column default (America/Sao_Paulo)
  std::string updatedBy;
};

struct FarmUpdate {
  std::string name;
  std::string locationWkt;
  std::string timezone; // empty = keep the current one
  std::string updatedBy;
};

class IFarmRepository {
public:
  virtual ~IFarmRepository() = default;

  [[nodiscard]] virtual std::vector<Farm> findAll() = 0;
  // By the internal id: only for other services (farm.ref), never from an API path
  [[nodiscard]] virtual std::optional<Farm> findById(const std::string& id) = 0;
  [[nodiscard]] virtual std::optional<Farm> findByPublicId(const std::string& publicId) = 0;
  // Returns the new farm's public id
  [[nodiscard]] virtual std::string create(const FarmCreate& data) = 0;
  // false = no farm with that public id
  [[nodiscard]] virtual bool update(const std::string& publicId, const FarmUpdate& data) = 0;
  [[nodiscard]] virtual bool remove(const std::string& publicId) = 0;
  // A name PostgreSQL knows (pg_timezone_names): the only list the daily aggregates can use
  [[nodiscard]] virtual bool isKnownTimezone(const std::string& name) = 0;
};

class FarmRepository : public IFarmRepository {
public:
  explicit FarmRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::vector<Farm> findAll() override;
  [[nodiscard]] std::optional<Farm> findById(const std::string& id) override;
  [[nodiscard]] std::optional<Farm> findByPublicId(const std::string& publicId) override;
  [[nodiscard]] std::string create(const FarmCreate& data) override;
  [[nodiscard]] bool update(const std::string& publicId, const FarmUpdate& data) override;
  [[nodiscard]] bool remove(const std::string& publicId) override;
  [[nodiscard]] bool isKnownTimezone(const std::string& name) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static Farm farmFromRow(rdws::database::IResultSet& rs);
  [[nodiscard]] std::optional<Farm> findOne(const std::string& column, const std::string& value);
};

} // namespace rdws::farm
