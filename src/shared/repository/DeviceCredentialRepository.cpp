#include "DeviceCredentialRepository.h"

#include "../crypto/credential_cipher.h"

namespace rdws::device {

namespace {

using rdws::crypto::fromHex;
using rdws::crypto::toHex;

// psk_key_enc is BYTEA; the shared IDatabase interface only carries text parameters,
// so the blob is hex-encoded on the way in (`decode($n, 'hex')`) and hex-decoded on
// the way out (`encode(psk_key_enc, 'hex')`) rather than widening IDatabase for one
// binary column.
constexpr auto kSelectCols =
    "SELECT id, device_id, psk_identity, encode(psk_key_enc, 'hex') AS psk_key_enc_hex, "
    "status, created_at, rotated_at, revoked_at FROM device_credentials";

} // namespace

DeviceCredential DeviceCredentialRepository::credentialFromRow(rdws::database::IResultSet& rs) {
  DeviceCredential c;
  c.id = rs.getString("id");
  c.deviceId = rs.getString("device_id");
  c.pskIdentity = rs.getString("psk_identity");
  c.pskKeyEnc = fromHex(rs.getString("psk_key_enc_hex"));
  c.status = rs.getString("status");
  c.createdAt = rs.getString("created_at");
  c.rotatedAt = rs.isNull("rotated_at") ? "" : rs.getString("rotated_at");
  c.revokedAt = rs.isNull("revoked_at") ? "" : rs.getString("revoked_at");
  return c;
}

std::string DeviceCredentialRepository::create(const std::string& deviceId,
                                                const std::vector<uint8_t>& pskKeyEnc) {
  const std::string query =
      "INSERT INTO device_credentials (device_id, psk_key_enc) "
      "VALUES ($1, decode($2, 'hex')) RETURNING psk_identity";
  const std::vector<std::string> params = {deviceId, toHex(pskKeyEnc)};

  auto rs = db_.execQuery(query, params);
  if (!rs->next()) {
    return {};
  }
  return rs->getString("psk_identity");
}

std::optional<DeviceCredential>
DeviceCredentialRepository::findActiveByDeviceId(const std::string& deviceId) {
  const std::string query =
      std::string(kSelectCols) + " WHERE device_id = $1 AND status = 'active'";
  const std::vector<std::string> params = {deviceId};

  auto rs = db_.execQuery(query, params);
  if (!rs->next()) {
    return std::nullopt;
  }
  return credentialFromRow(*rs);
}

std::vector<DeviceCredential> DeviceCredentialRepository::findAllActive() {
  const std::string query = std::string(kSelectCols) + " WHERE status = 'active'";

  auto rs = db_.execQuery(query, {});
  std::vector<DeviceCredential> credentials;
  while (rs->next()) {
    credentials.push_back(credentialFromRow(*rs));
  }
  return credentials;
}

bool DeviceCredentialRepository::rotate(const std::string& deviceId,
                                        const std::vector<uint8_t>& newPskKeyEnc) {
  const std::string query =
      "UPDATE device_credentials SET psk_key_enc = decode($1, 'hex'), rotated_at = now() "
      "WHERE device_id = $2 AND status = 'active'";
  const std::vector<std::string> params = {toHex(newPskKeyEnc), deviceId};
  return db_.execCommand(query, params);
}

bool DeviceCredentialRepository::revoke(const std::string& deviceId) {
  const std::string query =
      "UPDATE device_credentials SET status = 'revoked', revoked_at = now() "
      "WHERE device_id = $1 AND status = 'active'";
  const std::vector<std::string> params = {deviceId};
  return db_.execCommand(query, params);
}

} // namespace rdws::device
