// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rdws::firmware {

// What a device may download now
struct DeliveredImage {
  std::string buildId;
  std::string sha256;
  std::size_t size = 0;
};

class IDeliveryTargets {
public:
  virtual ~IDeliveryTargets() = default;

  // The image of the device's current target, if it may be downloaded. Throws when that can't be
  // known (database down).
  [[nodiscard]] virtual std::optional<DeliveredImage> find(const std::string& devicePublicId) = 0;
};

class IImageBlocks {
public:
  virtual ~IImageBlocks() = default;

  // `length` bytes of the image at `offset`; nullopt when the file is missing or doesn't have
  // exactly `size` bytes
  [[nodiscard]] virtual std::optional<std::vector<std::uint8_t>>
  readAt(const std::string& sha256, std::size_t size, std::size_t offset, std::size_t length) = 0;
};

// A block of an image: RFC 7959, where the size is 2^(szx + 4) and 7 is reserved
struct BlockNumber {
  std::uint32_t num = 0;
  unsigned szx = 6; // 1024 bytes
};
struct BlockRange {
  std::size_t offset = 0;
  std::size_t length = 0;
  bool more = false;
};

// nullopt past the end of the image, or with the reserved size
[[nodiscard]] std::optional<BlockRange> rangeOf(const BlockNumber& block, std::size_t imageSize);

struct DeliveryEvent {
  enum class Kind { Started, Finished, Abandoned };
  Kind kind = Kind::Started;
  std::string device;
  std::string buildId;
  std::size_t bytes = 0; // sent since the start, repeated blocks included
  std::chrono::milliseconds elapsed{0};
};

struct DeliveryReply {
  enum class Status {
    Content,     // 2.05
    BadBlock,    // 4.00
    NoImage,     // 4.04: no target, not enabled or withdrawn, all alike
    BrokenFile,  // 5.00
    Busy,        // 5.03, try in the next cycle
    Unavailable, // 5.03, try soon
  };
  Status status = Status::NoImage;
  BlockNumber block;
  BlockRange range;
  std::vector<std::uint8_t> data;
  std::array<std::uint8_t, 8> etag{}; // of the image: another one restarts the download
  std::size_t imageSize = 0;
  std::chrono::seconds retryAfter{0};
  std::vector<DeliveryEvent> events; // for the log
};

// Answers a device's request for a block of its image. Keeps nothing of the image: each block comes
// from the file, so a resume works at any block, and a new target shows at once in the ETag. Used
// from one thread.
class ImageDelivery {
public:
  using Clock = std::chrono::steady_clock;

  static constexpr std::chrono::seconds kTargetLifetime{5};
  static constexpr std::chrono::seconds kSlotIdle{30};
  static constexpr std::chrono::seconds kBusyRetry{3600}; // the next cycle
  static constexpr std::chrono::seconds kUnavailableRetry{30};

  ImageDelivery(IDeliveryTargets& targets, IImageBlocks& blocks, std::size_t maxDownloads)
      : targets_(targets), blocks_(blocks), maxDownloads_(maxDownloads) {}

  [[nodiscard]] DeliveryReply get(const std::string& devicePublicId, const BlockNumber& block,
                                  Clock::time_point now);

  [[nodiscard]] std::size_t downloads() const { return downloads_.size(); }

private:
  struct CachedTarget {
    std::optional<DeliveredImage> image;
    bool unknown = false; // asking failed
    Clock::time_point until;
  };
  struct Download {
    DeliveredImage image;
    std::size_t bytes = 0;
    Clock::time_point start;
    Clock::time_point last;
  };

  const CachedTarget& target(const std::string& device, Clock::time_point now);
  void dropIdle(Clock::time_point now, std::vector<DeliveryEvent>& events);
  void end(const std::string& device, DeliveryEvent::Kind kind, Clock::time_point now,
           std::vector<DeliveryEvent>& events);

  IDeliveryTargets& targets_;
  IImageBlocks& blocks_;
  std::size_t maxDownloads_;
  std::unordered_map<std::string, CachedTarget> cache_;
  std::unordered_map<std::string, Download> downloads_;
};

} // namespace rdws::firmware
