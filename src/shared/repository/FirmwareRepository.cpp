// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "FirmwareRepository.h"

#include <sstream>

namespace rdws::firmware {

namespace {

// targeted_by as one comma-separated text: the interface carries no arrays
constexpr auto kSelectImage =
    "SELECT i.id AS image_id, i.public_id, i.build_id, i.sha256, i.size_bytes, i.board_class, "
    "i.version, i.variant, coalesce(i.notes, '') AS notes, i.created_at, i.created_by, "
    "i.withdrawn_at, coalesce((SELECT string_agg(f.device_public_id::text, ',' "
    "ORDER BY f.device_id) FROM device_firmware_targets t "
    "JOIN device_firmware f ON f.device_id = t.device_id "
    "WHERE t.image_id = i.id AND t.replaced_at IS NULL), '') AS targeted_by "
    "FROM firmware_images i";

std::string textOrEmpty(rdws::database::IResultSet& rs, const std::string& column) {
  return rs.isNull(column) ? "" : rs.getString(column);
}

std::vector<std::string> split(const std::string& text) {
  std::vector<std::string> parts;
  std::istringstream in(text);
  for (std::string part; std::getline(in, part, ',');) {
    parts.push_back(part);
  }
  return parts;
}

} // namespace

FirmwareImage FirmwareRepository::imageFromRow(rdws::database::IResultSet& rs) {
  FirmwareImage i;
  i.id = rs.getString("image_id");
  i.publicId = rs.getString("public_id");
  i.buildId = rs.getString("build_id");
  i.sha256 = rs.getString("sha256");
  i.sizeBytes = rs.getInt("size_bytes");
  i.boardClass = rs.getString("board_class");
  i.version = rs.getString("version");
  i.variant = rs.getString("variant");
  i.notes = rs.getString("notes");
  i.createdAt = rs.getString("created_at");
  i.createdBy = rs.getString("created_by");
  i.withdrawnAt = textOrEmpty(rs, "withdrawn_at");
  i.targetedBy = split(rs.getString("targeted_by"));
  return i;
}

std::vector<FirmwareImage> FirmwareRepository::findImages() {
  auto rs = db_.execQuery(std::string(kSelectImage) + " ORDER BY i.id DESC");
  std::vector<FirmwareImage> images;
  while (rs->next()) {
    images.push_back(imageFromRow(*rs));
  }
  return images;
}

std::optional<FirmwareImage> FirmwareRepository::findImage(const std::string& publicId) {
  auto rs = db_.execQuery(std::string(kSelectImage) + " WHERE i.public_id = $1", {publicId});
  return rs->next() ? std::optional(imageFromRow(*rs)) : std::nullopt;
}

std::optional<FirmwareImage>
FirmwareRepository::findImageByIdentity(const std::string& buildId, const std::string& sha256) {
  auto rs = db_.execQuery(std::string(kSelectImage) +
                              " WHERE i.build_id = $1 OR i.sha256 = $2 ORDER BY i.id LIMIT 1",
                          {buildId, sha256});
  return rs->next() ? std::optional(imageFromRow(*rs)) : std::nullopt;
}

std::string FirmwareRepository::createImage(const FirmwareImageCreate& data) {
  auto rs = db_.execQuery(
      "INSERT INTO firmware_images (build_id, sha256, size_bytes, board_class, version, variant, "
      "notes, created_by) VALUES ($1, $2, $3, $4, $5, $6, nullif($7, ''), $8) RETURNING public_id",
      {data.info.buildId, data.info.sha256, std::to_string(data.info.size), data.info.imageClass,
       data.info.version, data.variant, data.notes, data.createdBy});
  return rs->next() ? rs->getString("public_id") : "";
}

bool FirmwareRepository::withdrawImage(const std::string& publicId, const std::string& by) {
  // The first withdrawal's time and author stay
  auto rs = db_.execQuery(
      "UPDATE firmware_images SET withdrawn_by = coalesce(withdrawn_by, $2), "
      "withdrawn_at = coalesce(withdrawn_at, now()) WHERE public_id = $1 RETURNING id",
      {publicId, by});
  return rs->next();
}

std::optional<DeviceFirmware> FirmwareRepository::findDevice(const std::string& devicePublicId) {
  auto rs = db_.execQuery("SELECT device_id, device_public_id, ota_enabled, ota_changed_at, "
                          "ota_changed_by FROM device_firmware WHERE device_public_id = $1",
                          {devicePublicId});
  if (!rs->next()) {
    return std::nullopt;
  }
  DeviceFirmware d;
  d.deviceId = rs->getString("device_id");
  d.devicePublicId = rs->getString("device_public_id");
  d.otaEnabled = rs->getBool("ota_enabled");
  d.otaChangedAt = textOrEmpty(*rs, "ota_changed_at");
  d.otaChangedBy = textOrEmpty(*rs, "ota_changed_by");

  auto target = db_.execQuery("SELECT c.image_id AS current_image, c.attempt, c.allow_bench, "
                              "c.set_at, c.set_by, x.* FROM device_firmware_targets c JOIN (" +
                                  std::string(kSelectImage) +
                                  ") x ON x.image_id = c.image_id "
                                  "WHERE c.device_id = $1 AND c.replaced_at IS NULL",
                              {d.deviceId});
  if (target->next()) {
    d.target = DeviceFirmware::Target{.image = imageFromRow(*target),
                                      .attempt = target->getInt("attempt"),
                                      .allowBench = target->getBool("allow_bench"),
                                      .setAt = target->getString("set_at"),
                                      .setBy = target->getString("set_by")};
  }
  return d;
}

bool FirmwareRepository::setOtaEnabled(const std::string& deviceId, bool enabled,
                                       const std::string& by) {
  auto rs = db_.execQuery("UPDATE device_firmware SET ota_enabled = $2, ota_changed_at = now(), "
                          "ota_changed_by = $3 WHERE device_id = $1 RETURNING device_id",
                          {deviceId, enabled ? "true" : "false", by});
  return rs->next();
}

void FirmwareRepository::setTarget(const std::string& deviceId,
                                   const std::optional<std::string>& imageId, bool allowBench,
                                   const std::string& by) {
  // The interface has no NULL parameter
  (void)db_.execQuery(
      "SELECT set_firmware_target($1::bigint, nullif($2, '')::bigint, $3::boolean, $4)",
      {deviceId, imageId.value_or(""), allowBench ? "true" : "false", by});
}

std::optional<int> FirmwareRepository::retryTarget(const std::string& deviceId) {
  auto rs = db_.execQuery("UPDATE device_firmware_targets SET attempt = attempt + 1 "
                          "WHERE device_id = $1 AND replaced_at IS NULL RETURNING attempt",
                          {deviceId});
  return rs->next() ? std::optional(rs->getInt("attempt")) : std::nullopt;
}

} // namespace rdws::firmware
