#include "crypto/credential_cipher.h"

#include <gtest/gtest.h>

using rdws::crypto::CredentialCipher;
using rdws::crypto::CredentialCipherError;

namespace {
std::string make32ByteKey(char fill = 'k') { return std::string(32, fill); }
} // namespace

TEST(CredentialCipherTest, EncryptDecryptRoundTrip) {
  CredentialCipher cipher(make32ByteKey());
  const std::string plaintext = "super-secret-psk-value";

  const auto blob = cipher.encrypt(plaintext);
  const auto decrypted = cipher.decrypt(blob);

  EXPECT_EQ(decrypted, plaintext);
}

TEST(CredentialCipherTest, CiphertextDoesNotContainPlaintext) {
  CredentialCipher cipher(make32ByteKey());
  const std::string plaintext = "super-secret-psk-value";

  const auto blob = cipher.encrypt(plaintext);
  const std::string blobStr(blob.begin(), blob.end());

  EXPECT_EQ(blobStr.find(plaintext), std::string::npos);
}

TEST(CredentialCipherTest, DifferentNoncePerEncryptCall) {
  CredentialCipher cipher(make32ByteKey());
  const std::string plaintext = "same-plaintext";

  const auto blob1 = cipher.encrypt(plaintext);
  const auto blob2 = cipher.encrypt(plaintext);

  EXPECT_NE(blob1, blob2);
}

TEST(CredentialCipherTest, RejectsWrongKeyLength) {
  EXPECT_THROW(CredentialCipher("too-short"), CredentialCipherError);
}

TEST(CredentialCipherTest, DecryptFailsWithWrongKey) {
  CredentialCipher cipher(make32ByteKey('a'));
  const auto blob = cipher.encrypt("some-secret");

  CredentialCipher wrongCipher(make32ByteKey('b'));
  EXPECT_THROW(wrongCipher.decrypt(blob), CredentialCipherError);
}

TEST(CredentialCipherTest, DecryptFailsOnTamperedCiphertext) {
  CredentialCipher cipher(make32ByteKey());
  auto blob = cipher.encrypt("some-secret");
  blob[blob.size() - 1] ^= 0xFF; // flip a byte inside the tag

  EXPECT_THROW(cipher.decrypt(blob), CredentialCipherError);
}
