// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../repository/FirmwareRepository.h"
#include "../types/service_result.h"
#include "client/IDeclaredFirmwareSource.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rdws::firmware {

// Where the image files are: `<sha256>.bin`, put there by the publish script (O19).
class IImageStore {
public:
  virtual ~IImageStore() = default;

  // nullopt when there is no such file. Never reads more than maxSize + 1 bytes: one byte over is
  // enough to refuse it. `sha256` is already known to be 64 hex digits.
  [[nodiscard]] virtual std::optional<std::vector<std::uint8_t>>
  read(const std::string& sha256, std::size_t maxSize) = 0;
};

// What a device's update is doing. Derived, never stored.
enum class UpdateState {
  None,        // no target
  Applied,     // it declares the target's build
  Rejected,    // it declares having refused this attempt of the target
  Disabled,    // a target, but no permission (O17)
  Downloading, // it declares progress
  Pending,     // a target it hasn't taken yet
};

[[nodiscard]] std::string toString(UpdateState state);

[[nodiscard]] UpdateState deriveState(const DeviceFirmware& device,
                                      const std::optional<DeclaredFirmware>& declared);

// What a station declares in the first pack of a cycle (O3)
struct OfferRequest {
  std::string build;                     // fw_build
  std::optional<std::string> boardClass; // fw_class
  std::optional<std::string> rejected;   // fw_rej
};

// The image a station should download, and the attempt it belongs to (O18)
struct FirmwareOffer {
  std::string build;
  int size = 0;
  std::string sha256;
  std::string boardClass;
  int attempt = 1;
};

// Why there's no offer: kept for the log
enum class NoOffer { NoTarget, Disabled, Withdrawn, Applied, OtherClass, Rejected };

[[nodiscard]] std::string toString(NoOffer reason);

struct OfferDecision {
  std::optional<FirmwareOffer> offer;
  NoOffer reason = NoOffer::NoTarget; // when there's none
};

[[nodiscard]] OfferDecision decideOffer(const DeviceFirmware& device, const OfferRequest& declared);

struct FirmwareStatus {
  DeviceFirmware device;
  std::optional<DeclaredFirmware> declared;
  UpdateState state = UpdateState::None;
};

struct TargetResult {
  std::string warning; // empty, or what couldn't be checked
};

struct RetryResult {
  int attempt = 1;
};

class FirmwareService {
public:
  FirmwareService(IFirmwareRepository& repo, IImageStore& store, IDeclaredFirmwareSource& declared)
      : repo_(repo), store_(store), declared_(declared) {}

  [[nodiscard]] std::vector<FirmwareImage> images() { return repo_.findImages(); }

  // Registers the file named by that SHA-256, with what the file itself says
  [[nodiscard]] rdws::types::ServiceResult<FirmwareImage>
  registerImage(const std::string& sha256, const std::string& variant, const std::string& notes,
                const std::string& by);
  [[nodiscard]] rdws::types::OperationResult withdraw(const std::string& imagePublicId,
                                                      const std::string& by);

  // No image clears the target
  [[nodiscard]] rdws::types::ServiceResult<TargetResult>
  setTarget(const std::string& devicePublicId, const std::optional<std::string>& imagePublicId,
            bool allowBench, const std::string& by);
  [[nodiscard]] rdws::types::ServiceResult<RetryResult> retry(const std::string& devicePublicId);
  [[nodiscard]] rdws::types::OperationResult setOtaEnabled(const std::string& devicePublicId,
                                                           bool enabled, const std::string& by);
  [[nodiscard]] rdws::types::ServiceResult<FirmwareStatus>
  status(const std::string& devicePublicId);
  // What the station that declared `declared` should download, if anything
  [[nodiscard]] rdws::types::ServiceResult<OfferDecision> offer(const std::string& devicePublicId,
                                                                const OfferRequest& declared);

private:
  IFirmwareRepository& repo_;
  IImageStore& store_;
  IDeclaredFirmwareSource& declared_;
};

} // namespace rdws::firmware
