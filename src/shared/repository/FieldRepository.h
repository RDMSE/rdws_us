// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>
#include <vector>

namespace rdws::field {

struct Field {
  std::string id;           // internal (BIGSERIAL): FK target, never sent to API clients
  std::string publicId;     // UUID: the id the API exposes and receives
  std::string farmId;       // internal
  std::string farmPublicId; // the farm's public id, copied on create (V19)
  std::string name;
  std::string area;     // empty if NULL
  std::string geometry; // WKT or empty
  std::string createdAt;
  std::string updatedAt;
  std::string updatedBy;
};

struct FieldCreate {
  std::string farmId;       // internal, resolved from farmPublicId by the service
  std::string farmPublicId;
  std::string name;
  std::string area; // empty = no area
  std::string updatedBy;
};

struct FieldUpdate {
  std::string name;
  std::string updatedBy;
};

class IFieldRepository {
public:
  virtual ~IFieldRepository() = default;

  // farmPublicId empty = every field
  [[nodiscard]] virtual std::vector<Field> findAll(const std::string& farmPublicId = {}) = 0;
  // By the internal id: only for other services (field.ref), never from an API path
  [[nodiscard]] virtual std::optional<Field> findById(const std::string& id) = 0;
  [[nodiscard]] virtual std::optional<Field> findByPublicId(const std::string& publicId) = 0;
  // Returns the new field's public id
  [[nodiscard]] virtual std::string create(const FieldCreate& data) = 0;
  // false = no field with that public id
  [[nodiscard]] virtual bool update(const std::string& publicId, const FieldUpdate& data) = 0;
  [[nodiscard]] virtual bool remove(const std::string& publicId) = 0;
};

class FieldRepository : public IFieldRepository {
public:
  explicit FieldRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::vector<Field> findAll(const std::string& farmPublicId = {}) override;
  [[nodiscard]] std::optional<Field> findById(const std::string& id) override;
  [[nodiscard]] std::optional<Field> findByPublicId(const std::string& publicId) override;
  [[nodiscard]] std::string create(const FieldCreate& data) override;
  [[nodiscard]] bool update(const std::string& publicId, const FieldUpdate& data) override;
  [[nodiscard]] bool remove(const std::string& publicId) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static Field fieldFromRow(rdws::database::IResultSet& rs);
  [[nodiscard]] std::optional<Field> findOne(const std::string& column, const std::string& value);
};

} // namespace rdws::field
