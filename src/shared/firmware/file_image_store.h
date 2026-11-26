// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../service/FirmwareService.h"
#include "image_delivery.h"

#include <filesystem>

namespace rdws::firmware {

// Images in a directory (a read-only volume), as `<sha256>.bin`. The publish script writes them
// under another name and renames them, so a file with this name is whole (O19).
class FileImageStore : public IImageStore, public IImageBlocks {
public:
  explicit FileImageStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

  [[nodiscard]] std::optional<std::vector<std::uint8_t>> read(const std::string& sha256,
                                                              std::size_t maxSize) override;
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> readAt(const std::string& sha256,
                                                                std::size_t size,
                                                                std::size_t offset,
                                                                std::size_t length) override;

private:
  std::filesystem::path directory_;
};

} // namespace rdws::firmware
