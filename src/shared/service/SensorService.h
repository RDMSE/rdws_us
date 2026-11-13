// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "../repository/SensorRepository.h"
#include "../types/service_result.h"
#include "client/IDeviceValidator.h"

namespace rdws::sensor {

class SensorService {
public:
  SensorService(ISensorRepository& repo, rdws::device::IDeviceValidator& deviceValidator)
      : repo_(repo), deviceValidator_(deviceValidator) {}

  // devicePublicId vazio = lista todos os sensors; preenchido mas inexistente = erro (404), não
  // lista vazia.
  [[nodiscard]] rdws::types::ServiceResult<std::vector<Sensor>>
  findAll(const std::string& devicePublicId = {});
  // By the internal id, for internal capabilities (sensor.ref)
  [[nodiscard]] std::optional<Sensor> findById(const std::string& id);
  [[nodiscard]] std::optional<Sensor> findByPublicId(const std::string& publicId);
  // data.devicePublicId is resolved to the internal device id here; returns the new public id
  [[nodiscard]] rdws::types::ServiceResult<std::string> create(SensorCreate data);
  // 404 when no sensor has that public id
  [[nodiscard]] rdws::types::OperationResult update(const std::string& publicId,
                                                    const SensorUpdate& data);
  // false = no sensor with that public id
  [[nodiscard]] bool remove(const std::string& publicId);
  // Every sensor with its internal ids, for IngestionService (sensor.list_owners)
  [[nodiscard]] std::vector<Sensor> listOwners();

private:
  ISensorRepository& repo_;
  rdws::device::IDeviceValidator& deviceValidator_;
};

} // namespace rdws::sensor
