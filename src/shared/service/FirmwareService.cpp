// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "FirmwareService.h"

#include <charconv>

namespace rdws::firmware {

using rdws::types::OperationResult;
using rdws::types::OperationStatus;
using rdws::types::ServiceResult;

namespace {

// SQLSTATEs of what another request did between our check and our write
constexpr auto kUniqueViolation = "23505";
constexpr auto kCheckViolation = "23514"; // raised by set_firmware_target (V23)

// "<build>/<attempt>/<cause>"; without a readable attempt, the first
struct Rejection {
  std::string build;
  int attempt = 1;
};

Rejection parseRejection(const std::string& text) {
  Rejection r;
  const auto slash = text.find('/');
  r.build = text.substr(0, slash);
  if (slash != std::string::npos) {
    int attempt = 0;
    const char* const begin = text.data() + slash + 1;
    const auto [end, error] = std::from_chars(begin, text.data() + text.size(), attempt);
    if (error == std::errc{} && end != begin && attempt > 0) {
      r.attempt = attempt;
    }
  }
  return r;
}

} // namespace

std::string toString(UpdateState state) {
  switch (state) {
  case UpdateState::None:
    return "none";
  case UpdateState::Applied:
    return "applied";
  case UpdateState::Rejected:
    return "rejected";
  case UpdateState::Disabled:
    return "disabled";
  case UpdateState::Downloading:
    return "downloading";
  case UpdateState::Pending:
    return "pending";
  }
  return "unknown";
}

UpdateState deriveState(const DeviceFirmware& device,
                        const std::optional<DeclaredFirmware>& declared) {
  if (!device.target) {
    return UpdateState::None;
  }
  const auto& target = *device.target;
  if (declared && declared->build == target.image.buildId) {
    return UpdateState::Applied;
  }
  if (declared && declared->rejected) {
    const auto rejection = parseRejection(*declared->rejected);
    // A retry raises the attempt: the rejection of an earlier one no longer counts (O18)
    if (rejection.build == target.image.buildId && rejection.attempt >= target.attempt) {
      return UpdateState::Rejected;
    }
  }
  if (!device.otaEnabled) {
    return UpdateState::Disabled;
  }
  if (declared && declared->percent) {
    return UpdateState::Downloading;
  }
  return UpdateState::Pending;
}

ServiceResult<FirmwareImage> FirmwareService::registerImage(const std::string& sha256,
                                                            const std::string& variant,
                                                            const std::string& notes,
                                                            const std::string& by) {
  using Result = ServiceResult<FirmwareImage>;
  if (!isSha256Hex(sha256)) {
    return Result::error("Invalid field: sha256 must be 64 lowercase hex digits", 400);
  }
  if (variant != "bench" && variant != "field") {
    return Result::error("Invalid field: variant must be 'bench' or 'field'", 400);
  }
  const auto file = store_.read(sha256, kMaxImageSize);
  if (!file) {
    return Result::error("No file with that SHA-256 in the image volume", 404);
  }
  const auto parsed = parseImage(*file);
  if (!parsed.image) {
    return Result::error("Not a valid image: " + parsed.error, 400);
  }
  if (parsed.image->sha256 != sha256) {
    return Result::error("The file's SHA-256 differs from its name", 400);
  }
  if (const auto existing = repo_.findImageByIdentity(parsed.image->buildId, sha256)) {
    return Result::error("Build " + existing->buildId + " is already registered", 409);
  }
  std::string publicId;
  try {
    publicId = repo_.createImage(
        {.info = *parsed.image, .variant = variant, .notes = notes, .createdBy = by});
  } catch (const rdws::database::DatabaseError& e) {
    if (e.sqlstate() != kUniqueViolation) {
      throw;
    }
    return Result::error("Build " + parsed.image->buildId + " is already registered", 409);
  }
  auto created = repo_.findImage(publicId);
  if (!created) {
    return Result::error("Failed to register the image", 500);
  }
  return Result::success(std::move(*created), 201);
}

OperationResult FirmwareService::withdraw(const std::string& imagePublicId, const std::string& by) {
  if (!repo_.withdrawImage(imagePublicId, by)) {
    return OperationResult::error("Image not found", 404);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Withdrawn"});
}

ServiceResult<TargetResult>
FirmwareService::setTarget(const std::string& devicePublicId,
                           const std::optional<std::string>& imagePublicId, bool allowBench,
                           const std::string& by) {
  using Result = ServiceResult<TargetResult>;
  const auto device = repo_.findDevice(devicePublicId);
  if (!device) {
    return Result::error("Device not found", 404);
  }
  if (!imagePublicId) {
    repo_.setTarget(device->deviceId, std::nullopt, false, by);
    return Result::success({});
  }

  const auto image = repo_.findImage(*imagePublicId);
  if (!image) {
    return Result::error("Image not found", 404);
  }
  if (!image->withdrawnAt.empty()) {
    return Result::error("The image is withdrawn", 409);
  }
  if (image->variant == "bench" && !allowBench) {
    return Result::error("Bench image (it has destructive console commands): set allowBench to "
                         "confirm",
                         409);
  }

  // The station's own class, when it declares one; its MCUboot refuses another anyway (O7)
  const auto lookup = declared_.declaredFirmware(devicePublicId);
  if (lookup.statusCode != 200) {
    return Result::error(lookup.errorMessage, lookup.statusCode);
  }
  TargetResult result;
  if (!lookup.declared || !lookup.declared->boardClass) {
    result.warning = "The device doesn't declare its board class: not checked here";
  } else if (*lookup.declared->boardClass != image->boardClass) {
    return Result::error("The image is for another board class", 409);
  }

  try {
    repo_.setTarget(device->deviceId, image->id, image->variant == "bench" && allowBench, by);
  } catch (const rdws::database::DatabaseError& e) {
    if (e.sqlstate() != kCheckViolation) {
      throw;
    }
    return Result::error("The image is withdrawn", 409);
  }
  return Result::success(std::move(result));
}

ServiceResult<RetryResult> FirmwareService::retry(const std::string& devicePublicId) {
  using Result = ServiceResult<RetryResult>;
  const auto device = repo_.findDevice(devicePublicId);
  if (!device) {
    return Result::error("Device not found", 404);
  }
  const auto attempt = repo_.retryTarget(device->deviceId);
  if (!attempt) {
    return Result::error("The device has no target", 409);
  }
  return Result::success({.attempt = *attempt});
}

OperationResult FirmwareService::setOtaEnabled(const std::string& devicePublicId, bool enabled,
                                               const std::string& by) {
  const auto device = repo_.findDevice(devicePublicId);
  if (!device || !repo_.setOtaEnabled(device->deviceId, enabled, by)) {
    return OperationResult::error("Device not found", 404);
  }
  return OperationResult::success(
      OperationStatus{.ok = true, .message = enabled ? "Enabled" : "Disabled"});
}

ServiceResult<FirmwareStatus> FirmwareService::status(const std::string& devicePublicId) {
  using Result = ServiceResult<FirmwareStatus>;
  auto device = repo_.findDevice(devicePublicId);
  if (!device) {
    return Result::error("Device not found", 404);
  }
  auto lookup = declared_.declaredFirmware(devicePublicId);
  if (lookup.statusCode != 200) {
    return Result::error(lookup.errorMessage, lookup.statusCode);
  }
  const auto state = deriveState(*device, lookup.declared);
  return Result::success(
      {.device = std::move(*device), .declared = std::move(lookup.declared), .state = state});
}

} // namespace rdws::firmware
