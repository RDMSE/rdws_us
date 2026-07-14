#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>
#include <vector>

namespace rdws::farm {

struct Farm {
  std::string id;
  std::string name;
  std::string location; // WKT or empty
  std::string createdAt;
  std::string updatedAt;
  std::string updatedBy;
};

struct FarmCreate {
  std::string name;
  std::string locationWkt; // empty = no geometry
  std::string updatedBy;
};

struct FarmUpdate {
  std::string name;
  std::string locationWkt;
  std::string updatedBy;
};

class IFarmRepository {
public:
  virtual ~IFarmRepository() = default;

  [[nodiscard]] virtual std::vector<Farm> findAll() = 0;
  [[nodiscard]] virtual std::optional<Farm> findById(const std::string& id) = 0;
  [[nodiscard]] virtual std::string create(const FarmCreate& data) = 0;
  [[nodiscard]] virtual bool update(const std::string& id, const FarmUpdate& data) = 0;
  [[nodiscard]] virtual bool remove(const std::string& id) = 0;
};

class FarmRepository : public IFarmRepository {
public:
  explicit FarmRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::vector<Farm> findAll() override;
  [[nodiscard]] std::optional<Farm> findById(const std::string& id) override;
  [[nodiscard]] std::string create(const FarmCreate& data) override;
  [[nodiscard]] bool update(const std::string& id, const FarmUpdate& data) override;
  [[nodiscard]] bool remove(const std::string& id) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static Farm farmFromRow(rdws::database::IResultSet& rs);
};

} // namespace rdws::farm
