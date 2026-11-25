// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "file_image_store.h"

#include "mcuboot_image.h"

#include <fstream>

namespace rdws::firmware {

std::optional<std::vector<std::uint8_t>> FileImageStore::read(const std::string& sha256,
                                                              std::size_t maxSize) {
  // Checked by the caller too: what makes a path here is never anything but hex digits
  if (!isSha256Hex(sha256)) {
    return std::nullopt;
  }
  const auto path = directory_ / (sha256 + ".bin");
  std::error_code error;
  // Not a link out of the volume, nor a directory or a device
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error) {
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> data(maxSize + 1);
  in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (in.bad()) {
    return std::nullopt;
  }
  data.resize(static_cast<std::size_t>(in.gcount()));
  return data;
}

} // namespace rdws::firmware
