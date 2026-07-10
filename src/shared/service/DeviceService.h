#pragma once
#include "../repository/DeviceRepository.h"
#include "../repository/FieldRepository.h"
#include "../types/service_result.h"

namespace rdws::device {

class DeviceService {
public:
  DeviceService(IDeviceRepository& repo, rdws::field::IFieldRepository& fieldRepo)
      : repo_(repo), fieldRepo_(fieldRepo) {}

  // fieldId vazio = lista todos os devices; fieldId preenchido mas inexistente = erro
  // (404), não lista vazia — sem isso, um field_id digitado errado silenciosamente
  // parece "esse campo não tem nenhum device" em vez de "esse campo não existe".
  [[nodiscard]] rdws::types::ServiceResult<std::vector<Device>>
  findAll(const std::string& fieldId = {});
  [[nodiscard]] std::optional<Device> findById(const std::string& id);
  [[nodiscard]] rdws::types::ServiceResult<std::string> create(const DeviceCreate& data);
  [[nodiscard]] rdws::types::OperationResult update(const std::string& id,
                                                    const DeviceUpdate& data);
  [[nodiscard]] rdws::types::OperationResult remove(const std::string& id);

private:
  IDeviceRepository& repo_;
  rdws::field::IFieldRepository& fieldRepo_;
};

} // namespace rdws::device
