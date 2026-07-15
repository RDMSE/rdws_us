#include "credential_cipher.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cctype>
#include <memory>

namespace rdws::crypto {

namespace {
constexpr int kKeyBytes = 32;   // AES-256
constexpr int kNonceBytes = 12; // GCM standard nonce size
constexpr int kTagBytes = 16;   // GCM standard tag size

struct EvpCtxDeleter {
  void operator()(EVP_CIPHER_CTX* ctx) const { EVP_CIPHER_CTX_free(ctx); }
};
using EvpCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCtxDeleter>;

} // namespace

CredentialCipher::CredentialCipher(const std::string& key) : key_(key) {
  if (key_.size() != kKeyBytes) {
    throw CredentialCipherError("CREDENTIAL_ENCRYPTION_KEY must be exactly 32 bytes (AES-256)");
  }
}

std::vector<uint8_t> CredentialCipher::encrypt(const std::string& plaintext) const {
  std::vector<uint8_t> nonce(kNonceBytes);
  if (RAND_bytes(nonce.data(), kNonceBytes) != 1) {
    throw CredentialCipherError("Failed to generate random nonce");
  }

  EvpCtxPtr ctx(EVP_CIPHER_CTX_new());
  if (!ctx) {
    throw CredentialCipherError("Failed to allocate cipher context");
  }

  if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) != 1 ||
      EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
                          reinterpret_cast<const unsigned char*>(key_.data()), nonce.data()) != 1) {
    throw CredentialCipherError("Failed to initialize AES-256-GCM encryption");
  }

  std::vector<uint8_t> ciphertext(plaintext.size());
  int outLen = 0;
  if (EVP_EncryptUpdate(ctx.get(), ciphertext.data(), &outLen,
                        reinterpret_cast<const unsigned char*>(plaintext.data()),
                        static_cast<int>(plaintext.size())) != 1) {
    throw CredentialCipherError("AES-256-GCM encryption failed");
  }
  int totalLen = outLen;

  if (EVP_EncryptFinal_ex(ctx.get(), ciphertext.data() + totalLen, &outLen) != 1) {
    throw CredentialCipherError("AES-256-GCM encryption finalization failed");
  }
  totalLen += outLen;
  ciphertext.resize(totalLen);

  std::vector<uint8_t> tag(kTagBytes);
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, kTagBytes, tag.data()) != 1) {
    throw CredentialCipherError("Failed to extract AES-256-GCM tag");
  }

  std::vector<uint8_t> blob;
  blob.reserve(nonce.size() + ciphertext.size() + tag.size());
  blob.insert(blob.end(), nonce.begin(), nonce.end());
  blob.insert(blob.end(), ciphertext.begin(), ciphertext.end());
  blob.insert(blob.end(), tag.begin(), tag.end());
  return blob;
}

std::string CredentialCipher::decrypt(const std::vector<uint8_t>& blob) const {
  if (blob.size() < static_cast<size_t>(kNonceBytes + kTagBytes)) {
    throw CredentialCipherError("Ciphertext blob too short to contain nonce + tag");
  }

  const uint8_t* nonce = blob.data();
  const uint8_t* ciphertext = blob.data() + kNonceBytes;
  const size_t ciphertextLen = blob.size() - kNonceBytes - kTagBytes;
  const uint8_t* tag = blob.data() + kNonceBytes + ciphertextLen;

  EvpCtxPtr ctx(EVP_CIPHER_CTX_new());
  if (!ctx) {
    throw CredentialCipherError("Failed to allocate cipher context");
  }

  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) != 1 ||
      EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                          reinterpret_cast<const unsigned char*>(key_.data()), nonce) != 1) {
    throw CredentialCipherError("Failed to initialize AES-256-GCM decryption");
  }

  std::vector<uint8_t> plaintext(ciphertextLen);
  int outLen = 0;
  if (ciphertextLen > 0 &&
      EVP_DecryptUpdate(ctx.get(), plaintext.data(), &outLen, ciphertext,
                        static_cast<int>(ciphertextLen)) != 1) {
    throw CredentialCipherError("AES-256-GCM decryption failed");
  }
  int totalLen = outLen;

  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kTagBytes,
                          const_cast<uint8_t*>(tag)) != 1) {
    throw CredentialCipherError("Failed to set AES-256-GCM tag");
  }

  if (EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + totalLen, &outLen) != 1) {
    throw CredentialCipherError("AES-256-GCM authentication failed (tampered ciphertext or wrong key)");
  }
  totalLen += outLen;
  plaintext.resize(totalLen);

  return std::string(plaintext.begin(), plaintext.end());
}

std::vector<uint8_t> generateRandomBytes(size_t count) {
  std::vector<uint8_t> bytes(count);
  if (RAND_bytes(bytes.data(), static_cast<int>(count)) != 1) {
    throw CredentialCipherError("Failed to generate random bytes");
  }
  return bytes;
}

std::string toHex(const std::vector<uint8_t>& bytes) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const uint8_t b : bytes) {
    hex.push_back(kHexDigits[b >> 4]);
    hex.push_back(kHexDigits[b & 0x0F]);
  }
  return hex;
}

std::string toHex(const std::string& bytes) {
  return toHex(std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

std::vector<uint8_t> fromHex(const std::string& hex) {
  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  auto nibble = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    return static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
  };
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  }
  return bytes;
}

} // namespace rdws::crypto
