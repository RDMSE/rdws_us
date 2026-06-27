#pragma once
#include "../repository/FieldRepository.h"

namespace rdws::field {

class FieldService {
public:
  explicit FieldService(IFieldRepository& repo) : repo_(repo) {}

  [[nodiscard]] std::vector<Field> findAll(const std::string& farmId = {});
  [[nodiscard]] std::optional<Field> findById(const std::string& id);
  [[nodiscard]] std::string create(const FieldCreate& data);
  [[nodiscard]] bool update(const std::string& id, const FieldUpdate& data);
  [[nodiscard]] bool remove(const std::string& id);

private:
  IFieldRepository& repo_;
};

} // namespace rdws::field
