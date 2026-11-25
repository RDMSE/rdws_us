// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../repository/FirmwareRepository.h"
#include "image_delivery.h"

namespace rdws::firmware {

// Who may download: the current target, of a device with permission, whose image wasn't withdrawn.
// Whether it's worth offering (a rejected one) isn't decided here.
class TargetDelivery : public IDeliveryTargets {
public:
  explicit TargetDelivery(IFirmwareRepository& repo) : repo_(repo) {}

  [[nodiscard]] std::optional<DeliveredImage> find(const std::string& devicePublicId) override {
    const auto device = repo_.findDevice(devicePublicId);
    if (!device || !device->otaEnabled || !device->target ||
        !device->target->image.withdrawnAt.empty() || device->target->image.sizeBytes <= 0) {
      return std::nullopt;
    }
    const auto& image = device->target->image;
    return DeliveredImage{.buildId = image.buildId,
                          .sha256 = image.sha256,
                          .size = static_cast<std::size_t>(image.sizeBytes)};
  }

private:
  IFirmwareRepository& repo_;
};

} // namespace rdws::firmware
