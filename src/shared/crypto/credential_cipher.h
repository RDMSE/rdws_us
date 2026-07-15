#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace rdws::crypto {

// AES-256-GCM encrypt/decrypt for secrets that must be recoverable in plaintext
// (unlike password hashes) — used to store device PSKs at rest
// (Plano_DeviceCredentials.md §3). Output/input format is a single blob:
// nonce(12) || ciphertext || tag(16).
class CredentialCipherError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class CredentialCipher {
public:
  // key must be exactly 32 bytes (AES-256). Throws CredentialCipherError otherwise.
  explicit CredentialCipher(const std::string& key);

  [[nodiscard]] std::vector<uint8_t> encrypt(const std::string& plaintext) const;
  [[nodiscard]] std::string decrypt(const std::vector<uint8_t>& blob) const;

private:
  std::string key_;
};

// CSPRNG bytes (OpenSSL RAND_bytes) — used to generate a fresh device PSK.
[[nodiscard]] std::vector<uint8_t> generateRandomBytes(size_t count);

// Shared by DeviceCredentialRepository (BYTEA <-> text param) and any capability
// handler that needs to hand raw key/ciphertext bytes to a JSON caller.
[[nodiscard]] std::string toHex(const std::vector<uint8_t>& bytes);
[[nodiscard]] std::vector<uint8_t> fromHex(const std::string& hex);
[[nodiscard]] std::string toHex(const std::string& bytes);

} // namespace rdws::crypto
