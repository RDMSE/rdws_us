// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "firmware/mcuboot_image.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <openssl/sha.h>
#include <string>
#include <vector>

using rdws::firmware::isSha256Hex;
using rdws::firmware::kMaxImageSize;
using rdws::firmware::parseImage;
using rdws::firmware::sha256Hex;

namespace {

using Buffer = std::vector<std::uint8_t>;

void put16(Buffer& b, std::uint32_t v) {
  b.push_back(v & 0xff);
  b.push_back(v >> 8 & 0xff);
}

void put32(Buffer& b, std::uint32_t v) {
  put16(b, v & 0xffff);
  put16(b, v >> 16);
}

void set32(Buffer& b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; i++) {
    b[at + i] = v >> (8 * i) & 0xff;
  }
}

void putTlv(Buffer& b, std::uint16_t type, const Buffer& value) {
  put16(b, type);
  put16(b, value.size());
  b.insert(b.end(), value.begin(), value.end());
}

const Buffer kClass = {0xef, 0x76, 0xf5, 0x9e, 0x3b, 0xe9, 0x48, 0x1e,
                       0x92, 0x23, 0x71, 0x23, 0x96, 0x33, 0xd8, 0xe3};

// A synthetic image, as imgtool lays it out. Each test changes one thing.
struct Image {
  std::uint32_t magic = 0x96f3b83d;
  std::uint16_t headerSize = 64;
  std::uint32_t flags = 0;
  int buildIds = 1;
  bool withClass = true;
  bool classProtected = true;
  Buffer classValue = kClass;
  int hashes = 1;
  std::size_t filler = 300;

  [[nodiscard]] Buffer build() const {
    Buffer payload(filler, 0xa5);
    for (int i = 0; i < buildIds; i++) {
      const Buffer note = {4, 0, 0, 0, 20, 0, 0, 0, 3, 0, 0, 0, 'G', 'N', 'U', 0};
      payload.insert(payload.end(), note.begin(), note.end());
      for (std::uint8_t k = 0; k < 20; k++) {
        payload.push_back(0x10 + k);
      }
      payload.insert(payload.end(), 40, 0x5a);
    }

    Buffer prot;
    if (withClass && classProtected) {
      Buffer tlvs;
      putTlv(tlvs, 0x75, classValue);
      put16(prot, 0x6908);
      put16(prot, tlvs.size() + 4);
      prot.insert(prot.end(), tlvs.begin(), tlvs.end());
    }

    Buffer file;
    put32(file, magic);
    put32(file, 0);
    put16(file, headerSize);
    put16(file, prot.size());
    put32(file, payload.size());
    put32(file, flags);
    file.push_back(1);
    file.push_back(2);
    put16(file, 3);
    put32(file, 45);
    put32(file, 0);
    file.resize(headerSize, 0);
    file.insert(file.end(), payload.begin(), payload.end());
    file.insert(file.end(), prot.begin(), prot.end());

    Buffer digest(SHA256_DIGEST_LENGTH);
    SHA256(file.data(), file.size(), digest.data());
    Buffer tlvs;
    for (int i = 0; i < hashes; i++) {
      putTlv(tlvs, 0x10, digest);
    }
    if (withClass && !classProtected) {
      putTlv(tlvs, 0x75, classValue);
    }
    putTlv(tlvs, 0x01, Buffer(32, 0x44)); // key hash
    putTlv(tlvs, 0x22, Buffer(70, 0x30)); // signature
    put16(file, 0x6907);
    put16(file, tlvs.size() + 4);
    file.insert(file.end(), tlvs.begin(), tlvs.end());
    return file;
  }
};

std::string refusal(const Buffer& file, std::size_t maxSize = kMaxImageSize) {
  const auto result = parseImage(file, maxSize);
  EXPECT_FALSE(result.image.has_value());
  EXPECT_FALSE(result.error.empty());
  return result.error;
}

} // namespace

TEST(McubootImage, ReadsWhatTheImageSays) {
  const auto file = Image{}.build();
  const auto result = parseImage(file);
  ASSERT_TRUE(result.image.has_value()) << result.error;
  EXPECT_EQ(result.image->size, file.size());
  EXPECT_EQ(result.image->sha256, sha256Hex(file));
  EXPECT_EQ(result.image->buildId, "1011121314151617");
  EXPECT_EQ(result.image->imageClass, "ef76f59e-3be9-481e-9223-71239633d8e3");
  EXPECT_EQ(result.image->version, "1.2.3+45");
  EXPECT_TRUE(result.error.empty());
}

TEST(McubootImage, EveryTruncationIsRefused) {
  const auto file = Image{}.build();
  for (std::size_t n = 0; n < file.size(); n++) {
    const Buffer cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n));
    EXPECT_FALSE(parseImage(cut).image.has_value()) << n << " of " << file.size() << " bytes";
  }
}

TEST(McubootImage, EveryFlippedByteOfTheSignedPartIsRefused) {
  const auto good = Image{}.build();
  const auto payloadSize = 300U + 16 + 20 + 40;
  const std::size_t hashed = 64 + payloadSize + 24;
  for (std::size_t i = 0; i < hashed; i++) {
    auto file = good;
    file[i] ^= 0xff;
    EXPECT_FALSE(parseImage(file).image.has_value()) << "byte " << i;
  }
}

TEST(McubootImage, WrongMagic) {
  EXPECT_EQ(refusal(Image{.magic = 0x96f3b83c}.build()), "not an MCUboot image");
  EXPECT_EQ(refusal(Buffer(5000, 0x7f)), "not an MCUboot image");
}

TEST(McubootImage, EmptyAndTiny) {
  EXPECT_EQ(refusal({}), "shorter than an image header");
  EXPECT_EQ(refusal(Buffer(31, 0)), "shorter than an image header");
}

TEST(McubootImage, SizesThatReachPastTheFileOrWrap) {
  auto file = Image{}.build();
  for (const std::uint32_t size : {0xffffffffU, 0xffffffc0U, 0x80000000U, 1'000'000U}) {
    auto bad = file;
    set32(bad, 12, size);
    EXPECT_NE(refusal(bad).find("truncated"), std::string::npos) << size;
  }
  auto zero = file;
  set32(zero, 12, 0);
  EXPECT_EQ(refusal(zero), "bad header");
  auto shortHeader = file;
  shortHeader[8] = 31;
  shortHeader[9] = 0;
  EXPECT_EQ(refusal(shortHeader), "bad header");
}

TEST(McubootImage, SizeLimit) {
  const auto file = Image{}.build();
  EXPECT_TRUE(parseImage(file, file.size()).image.has_value());
  EXPECT_NE(refusal(file, file.size() - 1).find("larger than"), std::string::npos);
  EXPECT_NE(refusal(Buffer(kMaxImageSize + 1, 0)).find("larger than"), std::string::npos);
}

TEST(McubootImage, TlvsThatReachPastTheirArea) {
  const auto file = Image{}.build();
  const std::size_t tail = file.size() - (4 + 36 + 36 + 74);
  for (const std::size_t lengthAt : {tail + 6, tail + 4 + 36 + 2}) { // hash and key hash lengths
    auto bad = file;
    bad[lengthAt] = 0xff;
    bad[lengthAt + 1] = 0xff;
    EXPECT_NE(refusal(bad).find("TLV"), std::string::npos);
  }
  auto total = file;
  total[tail + 2] ^= 0x01;
  EXPECT_NE(refusal(total).find("TLV"), std::string::npos);
  auto trailing = file;
  trailing.insert(trailing.end(), 16, 0xff); // padded for a slot
  EXPECT_NE(refusal(trailing).find("TLV"), std::string::npos);
}

TEST(McubootImage, Hash) {
  EXPECT_EQ(refusal(Image{.hashes = 0}.build()), "no SHA-256 in the image");
  EXPECT_EQ(refusal(Image{.hashes = 2}.build()), "no SHA-256 in the image");
  auto file = Image{}.build();
  file[file.size() - (36 + 74) - 1] ^= 1; // last byte of the hash TLV
  EXPECT_EQ(refusal(file), "image hash mismatch");
}

TEST(McubootImage, Class) {
  EXPECT_EQ(refusal(Image{.withClass = false}.build()), "no image class");
  EXPECT_EQ(refusal(Image{.classProtected = false}.build()), "no image class"); // not signed
  EXPECT_EQ(refusal(Image{.classValue = Buffer(15, 1)}.build()), "no image class");
}

TEST(McubootImage, BuildId) {
  EXPECT_EQ(refusal(Image{.buildIds = 0}.build()), "no build ID");
  EXPECT_EQ(refusal(Image{.buildIds = 2}.build()), "no build ID"); // ambiguous
}

TEST(McubootImage, Encrypted) {
  EXPECT_EQ(refusal(Image{.flags = 0x04}.build()), "encrypted image");
  EXPECT_EQ(refusal(Image{.flags = 0x08}.build()), "encrypted image");
}

TEST(McubootImage, Sha256HexNames) {
  EXPECT_TRUE(isSha256Hex(std::string(64, 'a')));
  EXPECT_TRUE(isSha256Hex(sha256Hex(Buffer{1, 2, 3})));
  EXPECT_FALSE(isSha256Hex(std::string(63, 'a')));
  EXPECT_FALSE(isSha256Hex(std::string(65, 'a')));
  EXPECT_FALSE(isSha256Hex(std::string(64, 'A')));
  EXPECT_FALSE(isSha256Hex(std::string(64, 'g')));
  EXPECT_FALSE(isSha256Hex("../" + std::string(61, 'a')));
  EXPECT_FALSE(isSha256Hex(std::string(63, 'a') + "/"));
}

// The station's own image, by hand: FIRMWARE_IMAGE=.../zephyr.signed.bin FIRMWARE_BUILD=<fw_build>
TEST(McubootImage, RealImage) {
  const char* path = std::getenv("FIRMWARE_IMAGE");
  if (path == nullptr) {
    GTEST_SKIP() << "FIRMWARE_IMAGE not set";
  }
  std::ifstream in(path, std::ios::binary);
  ASSERT_TRUE(in.good()) << path;
  const Buffer file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto result = parseImage(file);
  ASSERT_TRUE(result.image.has_value()) << result.error;
  RecordProperty("build", result.image->buildId);
  if (const char* build = std::getenv("FIRMWARE_BUILD")) {
    EXPECT_EQ(result.image->buildId, build);
  }
  EXPECT_EQ(result.image->imageClass, "ef76f59e-3be9-481e-9223-71239633d8e3");
}
