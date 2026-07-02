#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>
#include <vector>

namespace rdws::field {

struct Field {
  std::string id;
  std::string farmId;
  std::string name;
  std::string area;     // empty if NULL
  std::string geometry; // WKT or empty
  std::string createdAt;
  std::string updatedAt;
  std::string updatedBy;
};

struct FieldCreate {
  std::string farmId;
  std::string name;
  std::string area; // empty = no area
};

struct FieldUpdate {
  std::string name;
};

class IFieldRepository {
public:
  virtual ~IFieldRepository() = default;

  [[nodiscard]] virtual std::vector<Field> findAll(const std::string& farmId = {}) = 0;
  [[nodiscard]] virtual std::optional<Field> findById(const std::string& id) = 0;
  [[nodiscard]] virtual std::string create(const FieldCreate& data) = 0;
  [[nodiscard]] virtual bool update(const std::string& id, const FieldUpdate& data) = 0;
  [[nodiscard]] virtual bool remove(const std::string& id) = 0;
};

class FieldRepository : public IFieldRepository {
public:
  explicit FieldRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::vector<Field> findAll(const std::string& farmId = {}) override;
  [[nodiscard]] std::optional<Field> findById(const std::string& id) override;
  [[nodiscard]] std::string create(const FieldCreate& data) override;
  [[nodiscard]] bool update(const std::string& id, const FieldUpdate& data) override;
  [[nodiscard]] bool remove(const std::string& id) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static Field fieldFromRow(rdws::database::IResultSet& rs);
};

} // namespace rdws::field
