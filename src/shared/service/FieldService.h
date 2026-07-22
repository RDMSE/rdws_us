#pragma once
#include "../repository/FieldRepository.h"
#include "client/IFarmValidator.h"
#include "../types/service_result.h"

namespace rdws::field {

class FieldService {
public:
  FieldService(IFieldRepository& repo, rdws::farm::IFarmValidator& farmValidator)
      : repo_(repo), farmValidator_(farmValidator) {}

  // farmId vazio = lista todos os fields; farmId preenchido mas inexistente = erro
  // (404), não lista vazia — sem isso, um farm_id digitado errado silenciosamente
  // parece "essa fazenda não tem nenhum field" em vez de "essa fazenda não existe".
  [[nodiscard]] rdws::types::ServiceResult<std::vector<Field>>
  findAll(const std::string& farmId = {});
  [[nodiscard]] std::optional<Field> findById(const std::string& id);
  [[nodiscard]] rdws::types::ServiceResult<std::string> create(const FieldCreate& data);
  [[nodiscard]] bool update(const std::string& id, const FieldUpdate& data);
  [[nodiscard]] bool remove(const std::string& id);

private:
  IFieldRepository& repo_;
  rdws::farm::IFarmValidator& farmValidator_;
};

} // namespace rdws::field
