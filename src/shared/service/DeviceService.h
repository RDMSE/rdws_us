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

  // fieldPublicId vazio = lista todos os devices; preenchido mas inexistente = erro (404), não
  // lista vazia — sem isso, um field_id digitado errado silenciosamente parece "esse campo não
  // tem nenhum device" em vez de "esse campo não existe".
  [[nodiscard]] rdws::types::ServiceResult<std::vector<Device>>
  findAll(const std::string& fieldPublicId = {});
  // By the internal id, for internal capabilities
  [[nodiscard]] std::optional<Device> findById(const std::string& id);
  [[nodiscard]] std::optional<Device> findByPublicId(const std::string& publicId);
  // data.fieldPublicId is resolved to the internal field id here; returns both new ids
  [[nodiscard]] rdws::types::ServiceResult<DeviceIds> create(DeviceCreate data);
  // 404 when no device has that public id
  [[nodiscard]] rdws::types::OperationResult update(const std::string& publicId,
                                                    const DeviceUpdate& data);
  // By the internal id (IngestionService, device.update_location)
  [[nodiscard]] rdws::types::OperationResult updateLocation(const std::string& id,
                                                            const std::string& locationWkt);
  [[nodiscard]] rdws::types::OperationResult remove(const std::string& publicId);
  // Latest diagnostics snapshot of a device found by the caller; an empty optional when it
  // never sent any. The handler looks the device up (404 when missing).
  [[nodiscard]] std::optional<rdws::device_telemetry::DiagnosticsSnapshot>
  diagnostics(const Device& device);

private:
  IDeviceRepository& repo_;
  rdws::field::IFieldValidator& fieldValidator_;
  rdws::device_telemetry::IDeviceTelemetryRepository& telemetry_;
};

} // namespace rdws::device
