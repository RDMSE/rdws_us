#pragma once

#include "../crypto/credential_cipher.h"
#include "../repository/DeviceCredentialRepository.h"
#include "../types/service_result.h"

namespace rdws::device {

// PSK provisioning/consumption for CoAP/DTLS device auth (Plano_DeviceCredentials.md).
// Internal-only: capabilities backed by this service are registered on ServiceIdentity
// but deliberately absent from routes.json, so they are never reachable via HTTP —
// only via ServiceClient::invoke from another backend service.
struct ProvisionedCredential {
  std::string pskIdentity;
  std::string pskKeyPlaintext; // raw key bytes — returned once, never persisted in plaintext
};

struct ActiveCredential {
  std::string pskIdentity;
  std::string pskKeyPlaintext; // raw key bytes, decrypted just-in-time
};

class DeviceCredentialService {
public:
  DeviceCredentialService(IDeviceCredentialRepository& repo, rdws::crypto::CredentialCipher& cipher)
      : repo_(repo), cipher_(cipher) {}

  [[nodiscard]] rdws::types::ServiceResult<ProvisionedCredential>
  provision(const std::string& deviceId);
  [[nodiscard]] rdws::types::ServiceResult<ActiveCredential> getActive(const std::string& deviceId);
  [[nodiscard]] rdws::types::ServiceResult<ProvisionedCredential> rotate(const std::string& deviceId);
  [[nodiscard]] rdws::types::OperationResult revoke(const std::string& deviceId);

private:
  IDeviceCredentialRepository& repo_;
  rdws::crypto::CredentialCipher& cipher_;

  static constexpr size_t kPskKeyBytes = 32;
};

} // namespace rdws::device
