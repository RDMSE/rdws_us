#pragma once
#include "../repository/FarmRepository.h"

namespace rdws::farm {

class FarmService {
public:
  explicit FarmService(IFarmRepository& repo) : repo_(repo) {}

  [[nodiscard]] std::vector<Farm> findAll();
  [[nodiscard]] std::optional<Farm> findById(const std::string& id);
  [[nodiscard]] std::string create(const FarmCreate& data);
  [[nodiscard]] bool update(const std::string& id, const FarmUpdate& data);
  [[nodiscard]] bool remove(const std::string& id);

private:
  IFarmRepository& repo_;
};

} // namespace rdws::farm
