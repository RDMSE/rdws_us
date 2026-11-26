// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "file_image_store.h"

#include "mcuboot_image.h"

#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

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

std::optional<std::vector<std::uint8_t>> FileImageStore::readAt(const std::string& sha256,
                                                                std::size_t size,
                                                                std::size_t offset,
                                                                std::size_t length) {
  if (!isSha256Hex(sha256) || offset > size || length > size - offset) {
    return std::nullopt;
  }
  struct File {
    int fd;
    ~File() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  };
  const auto path = directory_ / (sha256 + ".bin");
  const File file{::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
  struct stat info{};
  if (file.fd < 0 || ::fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode) ||
      static_cast<std::uint64_t>(info.st_size) != size) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> data(length);
  std::size_t done = 0;
  while (done < length) {
    const auto n =
        ::pread(file.fd, data.data() + done, length - done, static_cast<off_t>(offset + done));
    if (n <= 0) {
      return std::nullopt;
    }
    done += static_cast<std::size_t>(n);
  }
  return data;
}

} // namespace rdws::firmware
