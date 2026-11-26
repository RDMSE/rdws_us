// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "image_delivery.h"

#include <algorithm>
#include <exception>

namespace rdws::firmware {

namespace {

using Kind = DeliveryEvent::Kind;
using Status = DeliveryReply::Status;

constexpr unsigned kReservedSzx = 7;

std::array<std::uint8_t, 8> etagOf(const std::string& sha256) {
  std::array<std::uint8_t, 8> etag{};
  for (std::size_t i = 0; i < etag.size() && 2 * i + 1 < sha256.size(); ++i) {
    etag[i] = static_cast<std::uint8_t>(std::stoul(sha256.substr(2 * i, 2), nullptr, 16));
  }
  return etag;
}

std::chrono::milliseconds since(ImageDelivery::Clock::time_point start,
                                ImageDelivery::Clock::time_point now) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
}

} // namespace

std::optional<BlockRange> rangeOf(const BlockNumber& block, std::size_t imageSize) {
  if (block.szx >= kReservedSzx) {
    return std::nullopt;
  }
  const std::size_t blockSize = std::size_t{1} << (block.szx + 4);
  const std::uint64_t offset = std::uint64_t{block.num} * blockSize;
  if (offset >= imageSize) {
    return std::nullopt;
  }
  const std::size_t length = std::min(blockSize, imageSize - static_cast<std::size_t>(offset));
  return BlockRange{.offset = static_cast<std::size_t>(offset),
                    .length = length,
                    .more = offset + length < imageSize};
}

const ImageDelivery::CachedTarget& ImageDelivery::target(const std::string& device,
                                                         Clock::time_point now) {
  auto& cached = cache_[device];
  if (now < cached.until) {
    return cached;
  }
  cached.until = now + kTargetLifetime;
  try {
    cached.image = targets_.find(device);
    cached.unknown = false;
  } catch (const std::exception&) {
    // Kept for as long: a database that takes long to fail can't be asked again at every block
    cached.unknown = true;
  }
  return cached;
}

void ImageDelivery::end(const std::string& device, Kind kind, Clock::time_point now,
                        std::vector<DeliveryEvent>& events) {
  const auto it = downloads_.find(device);
  if (it == downloads_.end()) {
    return;
  }
  const auto& d = it->second;
  events.push_back({.kind = kind,
                    .device = device,
                    .buildId = d.image.buildId,
                    .bytes = d.bytes,
                    .elapsed = since(d.start, kind == Kind::Abandoned ? d.last : now)});
  downloads_.erase(it);
}

void ImageDelivery::dropIdle(Clock::time_point now, std::vector<DeliveryEvent>& events) {
  std::vector<std::string> idle;
  for (const auto& [device, d] : downloads_) {
    if (now - d.last >= kSlotIdle) {
      idle.push_back(device);
    }
  }
  for (const auto& device : idle) {
    end(device, Kind::Abandoned, now, events);
  }
}

DeliveryReply ImageDelivery::get(const std::string& device, const BlockNumber& block,
                                 Clock::time_point now) {
  DeliveryReply reply;
  reply.block = block;
  dropIdle(now, reply.events);

  const auto& allowed = target(device, now);
  if (allowed.unknown) {
    reply.status = Status::Unavailable;
    reply.retryAfter = kUnavailableRetry;
    return reply;
  }
  if (!allowed.image) {
    reply.status = Status::NoImage;
    end(device, Kind::Abandoned, now, reply.events);
    return reply;
  }
  const DeliveredImage& image = *allowed.image;
  const auto range = rangeOf(block, image.size);
  if (!range) {
    reply.status = Status::BadBlock;
    return reply;
  }

  if (const auto it = downloads_.find(device);
      it != downloads_.end() && it->second.image.sha256 != image.sha256) {
    end(device, Kind::Abandoned, now, reply.events); // the target changed: another download
  }
  // The last block takes no slot: its answer can get lost, and the one asked again would find the
  // slot already given to someone else
  if (range->more && !downloads_.contains(device)) {
    if (downloads_.size() >= maxDownloads_) {
      reply.status = Status::Busy;
      reply.retryAfter = kBusyRetry;
      return reply;
    }
    downloads_.emplace(device, Download{.image = image, .start = now, .last = now});
    reply.events.push_back(
        {.kind = Kind::Started, .device = device, .buildId = image.buildId});
  }

  auto data = blocks_.readAt(image.sha256, image.size, range->offset, range->length);
  if (!data || data->size() != range->length) {
    reply.status = Status::BrokenFile;
    end(device, Kind::Abandoned, now, reply.events);
    return reply;
  }
  if (const auto it = downloads_.find(device); it != downloads_.end()) {
    it->second.bytes += data->size();
    it->second.last = now;
  }
  if (!range->more) {
    end(device, Kind::Finished, now, reply.events);
  }
  reply.status = Status::Content;
  reply.range = *range;
  reply.data = std::move(*data);
  reply.etag = etagOf(image.sha256);
  reply.imageSize = image.size;
  return reply;
}

} // namespace rdws::firmware
