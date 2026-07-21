#include "DeviceCredentialService.h"

namespace rdws::device {

using rdws::types::OperationResult;
using rdws::types::OperationStatus;
using rdws::types::ServiceResult;

ServiceResult<ProvisionedCredential> DeviceCredentialService::provision(const std::string& deviceId) {
  try {
    const auto keyBytes = rdws::crypto::generateRandomBytes(kPskKeyBytes);
    const std::string plaintext(keyBytes.begin(), keyBytes.end());
    const auto blob = cipher_.encrypt(plaintext);

    const std::string pskIdentity = repo_.create(deviceId, blob);
    if (pskIdentity.empty()) {
      return ServiceResult<ProvisionedCredential>::error("Failed to provision device credential",
                                                          500);
    }
    return ServiceResult<ProvisionedCredential>::success({pskIdentity, plaintext}, 201);
  } catch (const std::exception& e) {
    return ServiceResult<ProvisionedCredential>::error(
        std::string("Failed to provision device credential: ") + e.what(), 500);
  }
}

ServiceResult<ActiveCredential> DeviceCredentialService::getActive(const std::string& deviceId) {
  const auto credential = repo_.findActiveByDeviceId(deviceId);
  if (!credential) {
    return ServiceResult<ActiveCredential>::error("No active credential for device " + deviceId,
                                                  404);
  }
  try {
    const std::string plaintext = cipher_.decrypt(credential->pskKeyEnc);
    return ServiceResult<ActiveCredential>::success({credential->pskIdentity, plaintext});
  } catch (const std::exception& e) {
    return ServiceResult<ActiveCredential>::error(
        std::string("Failed to decrypt device credential: ") + e.what(), 500);
  }
}

ServiceResult<ProvisionedCredential> DeviceCredentialService::rotate(const std::string& deviceId) {
  if (!repo_.findActiveByDeviceId(deviceId)) {
    return ServiceResult<ProvisionedCredential>::error(
        "No active credential for device " + deviceId, 404);
  }

  try {
    const auto keyBytes = rdws::crypto::generateRandomBytes(kPskKeyBytes);
    const std::string plaintext(keyBytes.begin(), keyBytes.end());
    const auto blob = cipher_.encrypt(plaintext);

    if (!repo_.rotate(deviceId, blob)) {
      return ServiceResult<ProvisionedCredential>::error("Failed to rotate device credential", 500);
    }

    const auto rotated = repo_.findActiveByDeviceId(deviceId);
    if (!rotated) {
      return ServiceResult<ProvisionedCredential>::error("Failed to rotate device credential", 500);
    }
    return ServiceResult<ProvisionedCredential>::success({rotated->pskIdentity, plaintext});
  } catch (const std::exception& e) {
    return ServiceResult<ProvisionedCredential>::error(
        std::string("Failed to rotate device credential: ") + e.what(), 500);
  }
}

OperationResult DeviceCredentialService::revoke(const std::string& deviceId) {
  if (!repo_.findActiveByDeviceId(deviceId)) {
    return OperationResult::error("No active credential for device " + deviceId, 404);
  }
  if (!repo_.revoke(deviceId)) {
    return OperationResult::error("Failed to revoke device credential", 500);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Revoked"});
}

} // namespace rdws::device
