// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "firmware/image_delivery.h"

#include <gtest/gtest.h>

#include <limits>
#include <map>
#include <stdexcept>

using namespace rdws::firmware;
using namespace std::chrono_literals;
using Kind = DeliveryEvent::Kind;
using Status = DeliveryReply::Status;

namespace {

const std::string kShaA = "0123456789abcdef" + std::string(48, 'a');
const std::string kShaB = "fedcba9876543210" + std::string(48, 'b');

class Targets : public IDeliveryTargets {
public:
  std::map<std::string, DeliveredImage> images;
  bool down = false;
  int asked = 0;

  std::optional<DeliveredImage> find(const std::string& device) override {
    ++asked;
    if (down) {
      throw std::runtime_error("database down");
    }
    const auto it = images.find(device);
    return it == images.end() ? std::nullopt : std::optional(it->second);
  }
};

class Blocks : public IImageBlocks {
public:
  std::map<std::string, std::vector<std::uint8_t>> files;
  bool shortRead = false;

  std::optional<std::vector<std::uint8_t>> readAt(const std::string& sha, std::size_t size,
                                                  std::size_t offset,
                                                  std::size_t length) override {
    const auto it = files.find(sha);
    if (it == files.end() || it->second.size() != size) {
      return std::nullopt;
    }
    const auto from = it->second.begin() + static_cast<std::ptrdiff_t>(offset);
    return std::vector<std::uint8_t>(from, from + static_cast<std::ptrdiff_t>(
                                                      shortRead ? length - 1 : length));
  }
};

std::vector<std::uint8_t> content(std::size_t size, std::uint8_t seed) {
  std::vector<std::uint8_t> data(size);
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = static_cast<std::uint8_t>(i * 31 + i / 256 + seed);
  }
  return data;
}

class ImageDeliveryTest : public ::testing::Test {
protected:
  Targets targets;
  Blocks blocks;
  ImageDelivery delivery{targets, blocks, 2};
  ImageDelivery::Clock::time_point now = ImageDelivery::Clock::time_point{} + 1h;

  void SetUp() override {
    blocks.files[kShaA] = content(5000, 1);
    blocks.files[kShaB] = content(3000, 2);
    for (const auto* device : {"d1", "d2", "d3"}) {
      targets.images[device] = {.buildId = "build-a", .sha256 = kShaA, .size = 5000};
    }
  }
  DeliveryReply get(const std::string& device, std::uint32_t num, unsigned szx = 6) {
    return delivery.get(device, {.num = num, .szx = szx}, now);
  }
  std::vector<std::uint8_t> all(const std::string& device, unsigned szx, std::uint32_t from = 0) {
    std::vector<std::uint8_t> out;
    for (std::uint32_t num = from;; ++num) {
      const auto r = get(device, num, szx);
      EXPECT_EQ(r.status, Status::Content);
      out.insert(out.end(), r.data.begin(), r.data.end());
      if (r.status != Status::Content || !r.range.more) {
        return out;
      }
    }
  }
};

} // namespace

TEST(BlockRange, Edges) {
  EXPECT_EQ(rangeOf({.num = 0, .szx = 6}, 5000)->length, 1024U);
  EXPECT_TRUE(rangeOf({.num = 3, .szx = 6}, 5000)->more);
  const auto last = rangeOf({.num = 4, .szx = 6}, 5000);
  EXPECT_EQ(last->offset, 4096U);
  EXPECT_EQ(last->length, 904U);
  EXPECT_FALSE(last->more);
  EXPECT_FALSE(rangeOf({.num = 5, .szx = 6}, 5000));

  // A size that is a multiple of the block: the last one is whole, and there is none after it
  EXPECT_EQ(rangeOf({.num = 1, .szx = 6}, 2048)->length, 1024U);
  EXPECT_FALSE(rangeOf({.num = 1, .szx = 6}, 2048)->more);
  EXPECT_FALSE(rangeOf({.num = 2, .szx = 6}, 2048));

  EXPECT_EQ(rangeOf({.num = 0, .szx = 0}, 5000)->length, 16U);
  EXPECT_EQ(rangeOf({.num = 0, .szx = 6}, 1)->length, 1U);
  EXPECT_FALSE(rangeOf({.num = 0, .szx = 7}, 5000));
  EXPECT_FALSE(rangeOf({.num = 0, .szx = 6}, 0));
  EXPECT_FALSE(rangeOf({.num = std::numeric_limits<std::uint32_t>::max(), .szx = 6}, 5000));
  EXPECT_FALSE(rangeOf({.num = 1U << 22, .szx = 6}, 5000)); // 2^32: 0 in 32 bits
}

TEST_F(ImageDeliveryTest, WholeImageInEverySize) {
  for (unsigned szx = 0; szx <= 6; ++szx) {
    EXPECT_EQ(all("d1", szx), blocks.files[kShaA]) << szx;
  }
}

TEST_F(ImageDeliveryTest, ResumesInTheMiddle) {
  const auto tail = all("d1", 5, 4); // 512 bytes, from byte 2048
  EXPECT_EQ(tail, std::vector<std::uint8_t>(blocks.files[kShaA].begin() + 2048,
                                            blocks.files[kShaA].end()));
}

TEST_F(ImageDeliveryTest, EtagAndSize) {
  const auto r = get("d1", 0);
  EXPECT_EQ(r.etag, (std::array<std::uint8_t, 8>{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}));
  EXPECT_EQ(r.imageSize, 5000U);
  EXPECT_EQ(get("d1", 4).etag, r.etag);
}

TEST_F(ImageDeliveryTest, BadBlocksTakeNoSlot) {
  EXPECT_EQ(get("d1", 5).status, Status::BadBlock);
  EXPECT_EQ(get("d1", 0, 7).status, Status::BadBlock);
  EXPECT_EQ(delivery.downloads(), 0U);
}

TEST_F(ImageDeliveryTest, NoImageForADeviceWithoutOne) {
  const auto r = get("stranger", 0);
  EXPECT_EQ(r.status, Status::NoImage);
  EXPECT_TRUE(r.data.empty());
  EXPECT_EQ(delivery.downloads(), 0U);
}

TEST_F(ImageDeliveryTest, EachDeviceGetsItsOwn) {
  targets.images["d2"] = {.buildId = "build-b", .sha256 = kShaB, .size = 3000};
  std::vector<std::uint8_t> one;
  std::vector<std::uint8_t> two;
  for (std::uint32_t num = 0; num < 5; ++num) {
    const auto a = get("d1", num);
    one.insert(one.end(), a.data.begin(), a.data.end());
    if (num < 3) {
      const auto b = get("d2", num);
      two.insert(two.end(), b.data.begin(), b.data.end());
    }
  }
  EXPECT_EQ(one, blocks.files[kShaA]);
  EXPECT_EQ(two, blocks.files[kShaB]);
}

TEST_F(ImageDeliveryTest, ANewTargetShowsWithinFiveSeconds) {
  const auto before = get("d1", 0).etag;
  targets.images["d1"] = {.buildId = "build-b", .sha256 = kShaB, .size = 3000};
  now += 4999ms;
  EXPECT_EQ(get("d1", 1).etag, before);
  now += 1ms;
  const auto r = get("d1", 1);
  EXPECT_NE(r.etag, before);
  EXPECT_EQ(r.imageSize, 3000U);
  ASSERT_EQ(r.events.size(), 2U);
  EXPECT_EQ(r.events[0].kind, Kind::Abandoned);
  EXPECT_EQ(r.events[0].buildId, "build-a");
  EXPECT_EQ(r.events[1].kind, Kind::Started);
  EXPECT_EQ(r.events[1].buildId, "build-b");
  EXPECT_EQ(delivery.downloads(), 1U);
}

TEST_F(ImageDeliveryTest, AWithdrawalStopsTheDownloadAndFreesTheSlot) {
  (void)get("d1", 0);
  targets.images.erase("d1");
  now += 5s;
  const auto r = get("d1", 1);
  EXPECT_EQ(r.status, Status::NoImage);
  ASSERT_EQ(r.events.size(), 1U);
  EXPECT_EQ(r.events[0].kind, Kind::Abandoned);
  EXPECT_EQ(delivery.downloads(), 0U);
}

TEST_F(ImageDeliveryTest, AskedOncePerFiveSeconds) {
  (void)all("d1", 2);
  EXPECT_EQ(targets.asked, 1);
  (void)get("stranger", 0);
  (void)get("stranger", 0);
  EXPECT_EQ(targets.asked, 2);
  now += 5s;
  (void)get("d1", 0);
  EXPECT_EQ(targets.asked, 3);
}

TEST_F(ImageDeliveryTest, DatabaseDownThenBack) {
  (void)get("d1", 0);
  targets.down = true;
  now += 5s;
  auto r = get("d1", 1);
  EXPECT_EQ(r.status, Status::Unavailable);
  EXPECT_EQ(r.retryAfter, ImageDelivery::kUnavailableRetry);
  (void)get("d1", 1);
  EXPECT_EQ(targets.asked, 2); // a failure isn't asked again at every block either
  EXPECT_EQ(delivery.downloads(), 1U);
  targets.down = false;
  now += 5s;
  r = get("d1", 1);
  EXPECT_EQ(r.status, Status::Content);
  EXPECT_TRUE(r.events.empty()); // the same download
}

TEST_F(ImageDeliveryTest, BusyBeyondTheLimit) {
  (void)get("d1", 0);
  (void)get("d2", 0);
  const auto r = get("d3", 0);
  EXPECT_EQ(r.status, Status::Busy);
  EXPECT_EQ(r.retryAfter, 3600s);
  EXPECT_TRUE(r.data.empty());
  EXPECT_EQ(get("d1", 1).status, Status::Content); // those who have one go on
}

TEST_F(ImageDeliveryTest, AFinishedDownloadFreesItsSlot) {
  (void)get("d2", 0);
  (void)all("d1", 6);
  EXPECT_EQ(delivery.downloads(), 1U);
  EXPECT_EQ(get("d3", 0).status, Status::Content);
}

TEST_F(ImageDeliveryTest, ASilentOneFreesItsSlot) {
  (void)get("d1", 0);
  (void)get("d2", 0);
  now += 29s;
  (void)get("d2", 1);
  EXPECT_EQ(get("d3", 0).status, Status::Busy);
  now += 1s;
  const auto r = get("d3", 0);
  EXPECT_EQ(r.status, Status::Content);
  ASSERT_EQ(r.events.size(), 2U);
  EXPECT_EQ(r.events[0].kind, Kind::Abandoned);
  EXPECT_EQ(r.events[0].device, "d1");
  EXPECT_EQ(r.events[1].device, "d3");
  EXPECT_EQ(get("d2", 2).status, Status::Content);
  EXPECT_EQ(get("d1", 1).status, Status::Busy);
}

// The answer to the last block can get lost
TEST_F(ImageDeliveryTest, TheLastBlockAgainEvenWhenFull) {
  (void)all("d1", 6);
  (void)get("d2", 0);
  (void)get("d3", 0);
  const auto r = get("d1", 4);
  EXPECT_EQ(r.status, Status::Content);
  EXPECT_TRUE(r.events.empty());
  EXPECT_EQ(delivery.downloads(), 2U);
}

TEST_F(ImageDeliveryTest, Events) {
  const auto first = get("d1", 0);
  ASSERT_EQ(first.events.size(), 1U);
  EXPECT_EQ(first.events[0].kind, Kind::Started);
  EXPECT_EQ(first.events[0].device, "d1");
  for (std::uint32_t num = 1; num < 4; ++num) {
    now += 100ms;
    EXPECT_TRUE(get("d1", num).events.empty());
  }
  now += 100ms;
  const auto last = get("d1", 4);
  ASSERT_EQ(last.events.size(), 1U);
  EXPECT_EQ(last.events[0].kind, Kind::Finished);
  EXPECT_EQ(last.events[0].buildId, "build-a");
  EXPECT_EQ(last.events[0].bytes, 5000U);
  EXPECT_EQ(last.events[0].elapsed, 400ms);
}

TEST_F(ImageDeliveryTest, AbandonedCountsUntilTheLastBlock) {
  (void)get("d1", 0);
  now += 2s;
  (void)get("d1", 1);
  now += 10min;
  const auto r = get("d2", 0);
  ASSERT_EQ(r.events.size(), 2U);
  EXPECT_EQ(r.events[0].kind, Kind::Abandoned);
  EXPECT_EQ(r.events[0].bytes, 2048U);
  EXPECT_EQ(r.events[0].elapsed, 2000ms);
}

TEST_F(ImageDeliveryTest, BrokenFile) {
  blocks.files[kShaA].pop_back();
  auto r = get("d1", 0);
  EXPECT_EQ(r.status, Status::BrokenFile);
  EXPECT_TRUE(r.data.empty());
  EXPECT_EQ(delivery.downloads(), 0U);
  blocks.files.clear();
  EXPECT_EQ(get("d1", 0).status, Status::BrokenFile);
  blocks.files[kShaA] = content(5000, 1);
  blocks.shortRead = true;
  EXPECT_EQ(get("d1", 0).status, Status::BrokenFile);
}
