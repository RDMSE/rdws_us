// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "../repository/DeviceRepository.h"
#include "../repository/DeviceTelemetryRepository.h"
#include "client/IFieldValidator.h"
#include "../types/service_result.h"

namespace rdws::device {

class DeviceService {
public:
  // telemetry: device_service owns reading a device's diagnostics (device.diagnostics); other
  // services get them through the gateway, not from the table (DeviceDiagnosticsClient).
  DeviceService(IDeviceRepository& repo, rdws::field::IFieldValidator& fieldValidator,
                rdws::device_telemetry::IDeviceTelemetryRepository& telemetry)
      : repo_(repo), fieldValidator_(fieldValidator), telemetry_(telemetry) {}

  // fieldId vazio = lista todos os devices; fieldId preenchido mas inexistente = erro
  // (404), não lista vazia — sem isso, um field_id digitado errado silenciosamente
  // parece "esse campo não tem nenhum device" em vez de "esse campo não existe".
  [[nodiscard]] rdws::types::ServiceResult<std::vector<Device>>
  findAll(const std::string& fieldId = {});
  [[nodiscard]] std::optional<Device> findById(const std::string& id);
  [[nodiscard]] rdws::types::ServiceResult<std::string> create(const DeviceCreate& data);
  [[nodiscard]] rdws::types::OperationResult update(const std::string& id,
                                                    const DeviceUpdate& data);
  [[nodiscard]] rdws::types::OperationResult updateLocation(const std::string& id,
                                                            const std::string& locationWkt);
  [[nodiscard]] rdws::types::OperationResult remove(const std::string& id);
  // Latest diagnostics snapshot; an empty optional when the device never sent any, 404 when the
  // device doesn't exist.
  [[nodiscard]] rdws::types::ServiceResult<std::optional<rdws::device_telemetry::DiagnosticsSnapshot>>
  diagnostics(const std::string& id);

private:
  IDeviceRepository& repo_;
  rdws::field::IFieldValidator& fieldValidator_;
  rdws::device_telemetry::IDeviceTelemetryRepository& telemetry_;
};

} // namespace rdws::device
