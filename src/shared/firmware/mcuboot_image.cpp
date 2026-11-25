// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "mcuboot_image.h"

#include <algorithm>
#include <array>
#include <openssl/evp.h>
#include <stdexcept>
#include <vector>

namespace rdws::firmware {

namespace {

using Bytes = std::span<const std::uint8_t>;

// bootutil/image.h (MCUboot)
constexpr std::uint32_t kImageMagic = 0x96f3b83d;
constexpr std::size_t kHeaderSize = 32;
constexpr std::uint16_t kTlvInfoMagic = 0x6907;
constexpr std::uint16_t kTlvProtInfoMagic = 0x6908;
constexpr std::size_t kTlvHeaderSize = 4;
constexpr std::uint16_t kTlvSha256 = 0x10;
constexpr std::uint16_t kTlvClass = 0x75;
constexpr std::uint32_t kFlagEncryptedAes128 = 0x04;
constexpr std::uint32_t kFlagEncryptedAes256 = 0x08;
constexpr std::size_t kSha256Size = 32;
constexpr std::size_t kClassSize = 16;

// An ELF note: name size 4, descriptor size 20 (SHA-1), type 3 (build ID), "GNU"
constexpr std::array<std::uint8_t, 16> kBuildIdNote = {4, 0, 0, 0, 20, 0,   0,   0,
                                                       3, 0, 0, 0, 'G', 'N', 'U', 0};
constexpr std::size_t kBuildIdSize = 20;
constexpr std::size_t kBuildIdShown = 8; // what the station sends as fw_build

std::uint16_t le16(Bytes b, std::size_t at) {
  return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}

std::uint32_t le32(Bytes b, std::size_t at) {
  return static_cast<std::uint32_t>(le16(b, at)) | static_cast<std::uint32_t>(le16(b, at + 2)) << 16;
}

std::string hex(Bytes b) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(b.size() * 2);
  for (const auto byte : b) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0xf]);
  }
  return out;
}

std::string uuid(Bytes b) {
  std::string out = hex(b);
  for (const std::size_t at : {20U, 16U, 12U, 8U}) {
    out.insert(at, 1, '-');
  }
  return out;
}

struct Tlv {
  std::uint16_t type;
  Bytes value;
};

// One TLV area: 4 bytes of magic and total size (both included in the total), then the TLVs, which
// must fill it exactly. nullopt if anything reaches past it.
std::optional<std::vector<Tlv>> readTlvs(Bytes area, std::uint16_t magic) {
  if (area.size() < kTlvHeaderSize || le16(area, 0) != magic || le16(area, 2) != area.size()) {
    return std::nullopt;
  }
  std::vector<Tlv> tlvs;
  std::size_t at = kTlvHeaderSize;
  while (at < area.size()) {
    if (area.size() - at < kTlvHeaderSize) {
      return std::nullopt;
    }
    const std::uint16_t type = le16(area, at);
    const std::size_t length = le16(area, at + 2);
    at += kTlvHeaderSize;
    if (area.size() - at < length) {
      return std::nullopt;
    }
    tlvs.push_back({type, area.subspan(at, length)});
    at += length;
  }
  return tlvs;
}

// The value of the only TLV of that type and size; nullopt if there is none, or more than one.
std::optional<Bytes> only(const std::vector<Tlv>& tlvs, std::uint16_t type, std::size_t size) {
  std::optional<Bytes> found;
  for (const auto& tlv : tlvs) {
    if (tlv.type != type) {
      continue;
    }
    if (found || tlv.value.size() != size) {
      return std::nullopt;
    }
    found = tlv.value;
  }
  return found;
}

// The build ID, when the payload has exactly one note
std::optional<Bytes> findBuildId(Bytes payload) {
  std::optional<Bytes> found;
  auto from = payload.begin();
  while (true) {
    const auto note = std::search(from, payload.end(), kBuildIdNote.begin(), kBuildIdNote.end());
    if (note == payload.end()) {
      return found;
    }
    const auto at = static_cast<std::size_t>(note - payload.begin()) + kBuildIdNote.size();
    if (found || payload.size() - at < kBuildIdSize) {
      return std::nullopt;
    }
    found = payload.subspan(at, kBuildIdSize);
    from = note + 1;
  }
}

std::array<std::uint8_t, kSha256Size> sha256(Bytes data) {
  std::array<std::uint8_t, kSha256Size> out{};
  unsigned int length = 0;
  if (EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_sha256(), nullptr) != 1 ||
      length != out.size()) {
    throw std::runtime_error("SHA-256 failed");
  }
  return out;
}

ParseResult refuse(std::string why) {
  return {.image = std::nullopt, .error = std::move(why)};
}

} // namespace

std::string sha256Hex(Bytes data) {
  return hex(sha256(data));
}

bool isSha256Hex(const std::string& value) {
  return value.size() == kSha256Size * 2 && std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

ParseResult parseImage(Bytes file, std::size_t maxSize) {
  if (file.size() < kHeaderSize) {
    return refuse("shorter than an image header");
  }
  if (file.size() > maxSize) {
    return refuse("larger than the station's slot: " + std::to_string(file.size()) + " of " +
                  std::to_string(maxSize) + " bytes");
  }
  if (le32(file, 0) != kImageMagic) {
    return refuse("not an MCUboot image");
  }

  // All of them up to 32 bits: added in 64, the sum can't wrap
  const std::uint64_t headerSize = le16(file, 8);
  const std::uint64_t protectedSize = le16(file, 10);
  const std::uint64_t payloadSize = le32(file, 12);
  const std::uint32_t flags = le32(file, 16);

  if (headerSize < kHeaderSize || payloadSize == 0) {
    return refuse("bad header");
  }
  if ((flags & (kFlagEncryptedAes128 | kFlagEncryptedAes256)) != 0) {
    return refuse("encrypted image");
  }
  const std::uint64_t hashed = headerSize + payloadSize + protectedSize;
  if (hashed > file.size()) {
    return refuse("truncated: its sizes reach past the end of the file");
  }

  const auto payload = file.subspan(headerSize, payloadSize);
  const auto protectedArea = file.subspan(headerSize + payloadSize, protectedSize);
  const auto tail = file.subspan(hashed);

  const auto tlvs = readTlvs(tail, kTlvInfoMagic);
  if (!tlvs) {
    return refuse("bad TLV area (truncated, padded, or not signed by imgtool)");
  }
  const auto imageHash = only(*tlvs, kTlvSha256, kSha256Size);
  if (!imageHash) {
    return refuse("no SHA-256 in the image");
  }
  const auto computed = sha256(file.first(hashed));
  if (!std::equal(computed.begin(), computed.end(), imageHash->begin())) {
    return refuse("image hash mismatch");
  }

  // The class only counts in the signed area
  const auto signedTlvs = readTlvs(protectedArea, kTlvProtInfoMagic);
  const auto imageClass =
      signedTlvs ? only(*signedTlvs, kTlvClass, kClassSize) : std::optional<Bytes>{};
  if (!imageClass) {
    return refuse("no image class");
  }
  const auto buildId = findBuildId(payload);
  if (!buildId) {
    return refuse("no build ID");
  }

  return {.image = ImageInfo{.size = file.size(),
                             .sha256 = sha256Hex(file),
                             .buildId = hex(buildId->first(kBuildIdShown)),
                             .imageClass = uuid(*imageClass),
                             .version = std::to_string(file[20]) + "." + std::to_string(file[21]) +
                                        "." + std::to_string(le16(file, 22)) + "+" +
                                        std::to_string(le32(file, 24))},
          .error = {}};
}

} // namespace rdws::firmware
