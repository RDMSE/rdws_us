// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../database/idatabase.h"
#include "../firmware/mcuboot_image.h"

#include <optional>
#include <string>
#include <vector>

namespace rdws::firmware {

struct FirmwareImage {
  std::string id; // internal
  std::string publicId;
  std::string buildId;
  std::string sha256;
  int sizeBytes = 0;
  std::string boardClass;
  std::string version;
  std::string variant; // "bench" | "field"
  std::string notes;
  std::string createdAt;
  std::string createdBy;
  std::string withdrawnAt; // empty while offered
  std::vector<std::string> targetedBy; // public ids of the devices it is the current target of
};

struct FirmwareImageCreate {
  ImageInfo info;
  std::string variant;
  std::string notes;
  std::string createdBy;
};

// A device's permission to update and its current target, if any
struct DeviceFirmware {
  std::string deviceId; // internal
  std::string devicePublicId;
  bool otaEnabled = false;
  std::string otaChangedAt;
  std::string otaChangedBy;

  struct Target {
    FirmwareImage image;
    int attempt = 1;
    bool allowBench = false;
    std::string setAt;
    std::string setBy;
  };
  std::optional<Target> target;
};

class IFirmwareRepository {
public:
  virtual ~IFirmwareRepository() = default;

  [[nodiscard]] virtual std::vector<FirmwareImage> findImages() = 0;
  [[nodiscard]] virtual std::optional<FirmwareImage> findImage(const std::string& publicId) = 0;
  // The image with that build ID or that file, if registered
  [[nodiscard]] virtual std::optional<FirmwareImage>
  findImageByIdentity(const std::string& buildId, const std::string& sha256) = 0;
  // The new image's public id
  [[nodiscard]] virtual std::string createImage(const FirmwareImageCreate& data) = 0;
  // False when there is no such image; already withdrawn counts as done
  [[nodiscard]] virtual bool withdrawImage(const std::string& publicId, const std::string& by) = 0;

  [[nodiscard]] virtual std::optional<DeviceFirmware>
  findDevice(const std::string& devicePublicId) = 0;
  [[nodiscard]] virtual bool setOtaEnabled(const std::string& deviceId, bool enabled,
                                           const std::string& by) = 0;
  // Replaces the current target; no image clears it
  virtual void setTarget(const std::string& deviceId, const std::optional<std::string>& imageId,
                         bool allowBench, const std::string& by) = 0;
  // The current target's new attempt; nullopt without a target
  [[nodiscard]] virtual std::optional<int> retryTarget(const std::string& deviceId) = 0;
};

class FirmwareRepository : public IFirmwareRepository {
public:
  explicit FirmwareRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::vector<FirmwareImage> findImages() override;
  [[nodiscard]] std::optional<FirmwareImage> findImage(const std::string& publicId) override;
  [[nodiscard]] std::optional<FirmwareImage>
  findImageByIdentity(const std::string& buildId, const std::string& sha256) override;
  [[nodiscard]] std::string createImage(const FirmwareImageCreate& data) override;
  [[nodiscard]] bool withdrawImage(const std::string& publicId, const std::string& by) override;

  [[nodiscard]] std::optional<DeviceFirmware>
  findDevice(const std::string& devicePublicId) override;
  [[nodiscard]] bool setOtaEnabled(const std::string& deviceId, bool enabled,
                                   const std::string& by) override;
  void setTarget(const std::string& deviceId, const std::optional<std::string>& imageId,
                 bool allowBench, const std::string& by) override;
  [[nodiscard]] std::optional<int> retryTarget(const std::string& deviceId) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static FirmwareImage imageFromRow(rdws::database::IResultSet& rs);
};

} // namespace rdws::firmware
