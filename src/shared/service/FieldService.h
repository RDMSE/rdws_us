// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "../repository/FieldRepository.h"
#include "client/IFarmValidator.h"
#include "../types/service_result.h"

namespace rdws::field {

class FieldService {
public:
  FieldService(IFieldRepository& repo, rdws::farm::IFarmValidator& farmValidator)
      : repo_(repo), farmValidator_(farmValidator) {}

  // farmPublicId vazio = lista todos os fields; preenchido mas inexistente = erro (404), não
  // lista vazia — sem isso, um farm_id digitado errado silenciosamente parece "essa fazenda
  // não tem nenhum field" em vez de "essa fazenda não existe".
  [[nodiscard]] rdws::types::ServiceResult<std::vector<Field>>
  findAll(const std::string& farmPublicId = {});
  // By the internal id, for other services (field.ref)
  [[nodiscard]] std::optional<Field> findById(const std::string& id);
  [[nodiscard]] std::optional<Field> findByPublicId(const std::string& publicId);
  // data.farmPublicId is resolved to the internal farm id here; returns the new public id
  [[nodiscard]] rdws::types::ServiceResult<std::string> create(FieldCreate data);
  // 404 when no field has that public id
  [[nodiscard]] rdws::types::OperationResult update(const std::string& publicId,
                                                    const FieldUpdate& data);
  // false = no field with that public id
  [[nodiscard]] bool remove(const std::string& publicId);

private:
  IFieldRepository& repo_;
  rdws::farm::IFarmValidator& farmValidator_;

  // found + internal id, or the API error message and status
  [[nodiscard]] rdws::farm::FarmValidation resolveFarm(const std::string& farmPublicId);
};

} // namespace rdws::field
