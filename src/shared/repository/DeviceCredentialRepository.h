#pragma once

#include "../database/idatabase.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rdws::device {

struct DeviceCredential {
  std::string id;
  std::string deviceId;
  std::string pskIdentity;
  std::vector<uint8_t> pskKeyEnc; // nonce || ciphertext || tag (see rdws::crypto::CredentialCipher)
  std::string status;             // "active" | "revoked"
  std::string createdAt;
  std::string rotatedAt; // empty if NULL
  std::string revokedAt; // empty if NULL
};

class IDeviceCredentialRepository {
public:
  virtual ~IDeviceCredentialRepository() = default;

  // Returns the generated psk_identity (empty string on failure).
  [[nodiscard]] virtual std::string create(const std::string& deviceId,
                                            const std::vector<uint8_t>& pskKeyEnc) = 0;
  [[nodiscard]] virtual std::optional<DeviceCredential>
  findActiveByDeviceId(const std::string& deviceId) = 0;
  [[nodiscard]] virtual bool rotate(const std::string& deviceId,
                                    const std::vector<uint8_t>& newPskKeyEnc) = 0;
  [[nodiscard]] virtual bool revoke(const std::string& deviceId) = 0;
};

class DeviceCredentialRepository : public IDeviceCredentialRepository {
public:
  explicit DeviceCredentialRepository(rdws::database::IDatabase& db) : db_(db) {}

  // Returns the generated psk_identity (empty string on failure).
  [[nodiscard]] std::string create(const std::string& deviceId,
                                    const std::vector<uint8_t>& pskKeyEnc) override;
  [[nodiscard]] std::optional<DeviceCredential>
  findActiveByDeviceId(const std::string& deviceId) override;
  [[nodiscard]] bool rotate(const std::string& deviceId,
                            const std::vector<uint8_t>& newPskKeyEnc) override;
  [[nodiscard]] bool revoke(const std::string& deviceId) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static DeviceCredential credentialFromRow(rdws::database::IResultSet& rs);
};

} // namespace rdws::device
