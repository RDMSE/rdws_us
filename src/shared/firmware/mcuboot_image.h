// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace rdws::firmware {

// Largest file a station can take: its slot1 (864 KB) less MCUboot's trailer, which is at the slot's
// end (Plano_OTA.md, O14). The firmware checks its own partition again.
inline constexpr std::size_t kMaxImageSize = 874'170;

// What a signed MCUboot image says about itself (Plano_OTA.md §3): nothing here comes from who
// published it.
struct ImageInfo {
  std::size_t size = 0;   // of the whole file
  std::string sha256;     // of the whole file, hex: what the station checks its download against
  std::string buildId;    // first 8 bytes of the GNU build ID, hex: the station's fw_build
  std::string imageClass; // board UUID (O7), from the signed (protected) TLVs
  std::string version;    // "0.1.0+0"
};

// Either an image or why it isn't one; never both.
struct ParseResult {
  std::optional<ImageInfo> image;
  std::string error;
};

// Parses a signed, unpadded image (imgtool's zephyr.signed.bin) and checks its hash TLV. Doesn't
// check the signature: the server has no key, and the station's MCUboot is the judge. Reads only
// inside `file`, whatever its fields say.
[[nodiscard]] ParseResult parseImage(std::span<const std::uint8_t> file,
                                     std::size_t maxSize = kMaxImageSize);

// Lowercase hex SHA-256.
[[nodiscard]] std::string sha256Hex(std::span<const std::uint8_t> data);

// 64 lowercase hex digits: what names an image file. Anything else never reaches a path.
[[nodiscard]] bool isSha256Hex(const std::string& value);

} // namespace rdws::firmware
