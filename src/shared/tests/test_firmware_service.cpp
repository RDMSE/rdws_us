// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "service/FirmwareService.h"

#include <gtest/gtest.h>

#include <map>
#include <openssl/sha.h>
#include <optional>
#include <string>
#include <vector>

using namespace rdws::firmware;
using rdws::database::DatabaseError;

namespace {

constexpr auto kDevice = "4884a5f7-0e3a-4976-a1d6-662908f185fa";
constexpr auto kClassL475 = "ef76f59e-3be9-481e-9223-71239633d8e3";
constexpr auto kClassL4S5 = "8a4b6d65-0647-47df-ae44-bd64f3a6fd17";
constexpr auto kBuild = "1011121314151617";

// A minimal signed image (test_mcuboot_image.cpp covers the format itself)
std::vector<std::uint8_t> imageFile() {
  std::vector<std::uint8_t> f;
  const auto put16 = [&](unsigned v) {
    f.push_back(v & 0xff);
    f.push_back(v >> 8 & 0xff);
  };
  const auto put32 = [&](std::uint32_t v) {
    put16(v & 0xffff);
    put16(v >> 16);
  };
  const std::vector<std::uint8_t> note = {4, 0, 0, 0, 20, 0, 0, 0, 3, 0, 0, 0, 'G', 'N', 'U', 0};
  put32(0x96f3b83d);
  put32(0);
  put16(32);
  put16(24);
  put32(16 + 20);
  put32(0);
  put32(1);
  put32(0);
  put32(0);
  f.insert(f.end(), note.begin(), note.end());
  for (int i = 0; i < 20; i++) {
    f.push_back(0x10 + i);
  }
  put16(0x6908);
  put16(24);
  put16(0x75);
  put16(16);
  for (const int b : {0xef, 0x76, 0xf5, 0x9e, 0x3b, 0xe9, 0x48, 0x1e, 0x92, 0x23, 0x71, 0x23, 0x96,
                      0x33, 0xd8, 0xe3}) {
    f.push_back(static_cast<std::uint8_t>(b));
  }
  std::vector<std::uint8_t> digest(SHA256_DIGEST_LENGTH);
  SHA256(f.data(), f.size(), digest.data());
  put16(0x6907);
  put16(4 + 36);
  put16(0x10);
  put16(32);
  f.insert(f.end(), digest.begin(), digest.end());
  return f;
}

class FakeRepo : public IFirmwareRepository {
public:
  std::map<std::string, FirmwareImage> images; // by public id
  std::optional<DeviceFirmware> device;
  std::optional<std::string> throwOnWrite; // SQLSTATE
  int created = 0;
  int targetsSet = 0;
  std::string lastBy;
  std::optional<std::string> lastImage;
  bool lastAllowBench = false;

  std::vector<FirmwareImage> findImages() override { return {}; }
  std::optional<FirmwareImage> findImage(const std::string& id) override {
    const auto it = images.find(id);
    return it == images.end() ? std::nullopt : std::optional(it->second);
  }
  std::optional<FirmwareImage> findImageByIdentity(const std::string& build,
                                                   const std::string& sha) override {
    for (const auto& [id, image] : images) {
      if (image.buildId == build || image.sha256 == sha) {
        return image;
      }
    }
    return std::nullopt;
  }
  std::string createImage(const FirmwareImageCreate& data) override {
    maybeThrow();
    created++;
    FirmwareImage i;
    i.id = "7";
    i.publicId = "new";
    i.buildId = data.info.buildId;
    i.sha256 = data.info.sha256;
    i.boardClass = data.info.imageClass;
    i.variant = data.variant;
    i.notes = data.notes;
    i.createdBy = data.createdBy;
    images["new"] = i;
    return "new";
  }
  bool withdrawImage(const std::string& id, const std::string& by) override {
    lastBy = by;
    return images.contains(id);
  }
  std::optional<DeviceFirmware> findDevice(const std::string& id) override {
    return id == kDevice ? device : std::nullopt;
  }
  bool setOtaEnabled(const std::string&, bool enabled, const std::string& by) override {
    device->otaEnabled = enabled;
    lastBy = by;
    return true;
  }
  void setTarget(const std::string&, const std::optional<std::string>& imageId, bool allowBench,
                 const std::string& by) override {
    maybeThrow();
    targetsSet++;
    lastImage = imageId;
    lastAllowBench = allowBench;
    lastBy = by;
  }
  std::optional<int> retryTarget(const std::string&) override {
    if (!device->target) {
      return std::nullopt;
    }
    return ++device->target->attempt;
  }

private:
  void maybeThrow() const {
    if (throwOnWrite) {
      throw DatabaseError("refused", *throwOnWrite);
    }
  }
};

class FakeStore : public IImageStore {
public:
  std::map<std::string, std::vector<std::uint8_t>> files;
  std::vector<std::string> asked;
  std::optional<std::vector<std::uint8_t>> read(const std::string& sha, std::size_t) override {
    asked.push_back(sha);
    const auto it = files.find(sha);
    return it == files.end() ? std::nullopt : std::optional(it->second);
  }
};

class FakeDeclared : public IDeclaredFirmwareSource {
public:
  DeclaredFirmwareLookup answer;
  DeclaredFirmwareLookup declaredFirmware(const std::string&) override { return answer; }
};

class FirmwareServiceTest : public ::testing::Test {
protected:
  FakeRepo repo;
  FakeStore store;
  FakeDeclared declared;
  FirmwareService svc{repo, store, declared};
  std::string sha;

  void SetUp() override {
    const auto file = imageFile();
    sha = sha256Hex(file);
    store.files[sha] = file;
    repo.device = DeviceFirmware{.deviceId = "12", .devicePublicId = kDevice};
    repo.images["field"] = image("field", "1", "field");
    repo.images["bench"] = image("bench", "2", "bench");
    repo.images["other"] = image("other", "3", "field", kClassL4S5);
    repo.images["gone"] = image("gone", "4", "field");
    repo.images["gone"].withdrawnAt = "2026-11-25";
    declares("aaaaaaaaaaaaaaaa");
  }

  static FirmwareImage image(const std::string& name, const std::string& id,
                             const std::string& variant, const std::string& cls = kClassL475) {
    FirmwareImage i;
    i.id = id;
    i.publicId = name;
    i.buildId = std::string(15, 'b') + id;
    i.sha256 = std::string(63, '0') + id;
    i.boardClass = cls;
    i.variant = variant;
    return i;
  }
  void declares(const std::string& build) {
    declared.answer = {.declared = DeclaredFirmware{.build = build}};
  }
  void targets(const std::string& name) {
    repo.device->target = DeviceFirmware::Target{.image = repo.images[name]};
  }
  [[nodiscard]] UpdateState state() const {
    return deriveState(*repo.device, declared.answer.declared);
  }
};

} // namespace

TEST_F(FirmwareServiceTest, RegistersWhatTheFileSays) {
  const auto r = svc.registerImage(sha, "bench", "first", "rafael");
  ASSERT_TRUE(r.isSuccess()) << r.getErrorMessage();
  EXPECT_EQ(r.getStatusCode(), 201);
  EXPECT_EQ(r.getData().buildId, kBuild);
  EXPECT_EQ(r.getData().boardClass, kClassL475);
  EXPECT_EQ(r.getData().variant, "bench");
  EXPECT_EQ(r.getData().createdBy, "rafael");
}

TEST_F(FirmwareServiceTest, RefusesBadRequestsWithoutTouchingTheStore) {
  for (const std::string& bad : {std::string("../../etc/passwd"), std::string(64, 'Z'),
                                 std::string(63, 'a'), std::string()}) {
    EXPECT_EQ(svc.registerImage(bad, "bench", "", "x").getStatusCode(), 400) << bad;
  }
  for (const std::string& variant : {std::string("prod"), std::string("Bench"), std::string()}) {
    EXPECT_EQ(svc.registerImage(sha, variant, "", "x").getStatusCode(), 400) << variant;
  }
  EXPECT_TRUE(store.asked.empty());
  EXPECT_EQ(repo.created, 0);
}

TEST_F(FirmwareServiceTest, RefusesMissingInvalidAndMisnamedFiles) {
  EXPECT_EQ(svc.registerImage(std::string(64, 'a'), "field", "", "x").getStatusCode(), 404);

  const std::string junk(64, 'b');
  store.files[junk] = std::vector<std::uint8_t>(5000, 1);
  EXPECT_EQ(svc.registerImage(junk, "field", "", "x").getStatusCode(), 400);

  const std::string misnamed(64, 'c');
  store.files[misnamed] = imageFile();
  const auto r = svc.registerImage(misnamed, "field", "", "x");
  EXPECT_EQ(r.getStatusCode(), 400);
  EXPECT_NE(r.getErrorMessage().find("differs from its name"), std::string::npos);

  const std::string big(64, 'd');
  store.files[big] = std::vector<std::uint8_t>(kMaxImageSize + 1, 0);
  EXPECT_EQ(svc.registerImage(big, "field", "", "x").getStatusCode(), 400);
  EXPECT_EQ(repo.created, 0);
}

TEST_F(FirmwareServiceTest, SameBuildTwice) {
  ASSERT_TRUE(svc.registerImage(sha, "field", "", "x").isSuccess());
  EXPECT_EQ(svc.registerImage(sha, "field", "", "x").getStatusCode(), 409);
  EXPECT_EQ(repo.created, 1);
}

TEST_F(FirmwareServiceTest, SameBuildAtOnce) {
  repo.throwOnWrite = "23505";
  EXPECT_EQ(svc.registerImage(sha, "field", "", "x").getStatusCode(), 409);
  repo.throwOnWrite = "57014"; // anything else isn't ours to explain
  EXPECT_THROW((void)svc.registerImage(sha, "field", "", "x"), DatabaseError);
}

TEST_F(FirmwareServiceTest, Withdraw) {
  EXPECT_TRUE(svc.withdraw("field", "rafael").isSuccess());
  EXPECT_EQ(repo.lastBy, "rafael");
  EXPECT_EQ(svc.withdraw("nope", "rafael").getStatusCode(), 404);
}

TEST_F(FirmwareServiceTest, SetsAndClearsATarget) {
  const auto r = svc.setTarget(kDevice, "field", false, "rafael");
  ASSERT_TRUE(r.isSuccess()) << r.getErrorMessage();
  EXPECT_EQ(repo.lastImage, "1");
  EXPECT_FALSE(repo.lastAllowBench);
  EXPECT_EQ(repo.lastBy, "rafael");
  ASSERT_TRUE(svc.setTarget(kDevice, std::nullopt, false, "rafael").isSuccess());
  EXPECT_EQ(repo.lastImage, std::nullopt);
  EXPECT_EQ(repo.targetsSet, 2);
}

TEST_F(FirmwareServiceTest, TargetNotFoundOrWithdrawn) {
  EXPECT_EQ(svc.setTarget("other-device", "field", false, "x").getStatusCode(), 404);
  EXPECT_EQ(svc.setTarget(kDevice, "nope", false, "x").getStatusCode(), 404);
  EXPECT_EQ(svc.setTarget(kDevice, "gone", false, "x").getStatusCode(), 409);
  EXPECT_EQ(repo.targetsSet, 0);
  repo.throwOnWrite = "23514"; // withdrawn between the check and the write
  EXPECT_EQ(svc.setTarget(kDevice, "field", false, "x").getStatusCode(), 409);
  repo.throwOnWrite = "57014";
  EXPECT_THROW((void)svc.setTarget(kDevice, "field", false, "x"), DatabaseError);
}

TEST_F(FirmwareServiceTest, BenchNeedsConfirmation) {
  EXPECT_EQ(svc.setTarget(kDevice, "bench", false, "x").getStatusCode(), 409);
  EXPECT_EQ(repo.targetsSet, 0);
  ASSERT_TRUE(svc.setTarget(kDevice, "bench", true, "rafael").isSuccess());
  EXPECT_TRUE(repo.lastAllowBench);
  EXPECT_EQ(repo.lastBy, "rafael");
  // Nothing was allowed for a field image: the flag isn't recorded
  ASSERT_TRUE(svc.setTarget(kDevice, "field", true, "rafael").isSuccess());
  EXPECT_FALSE(repo.lastAllowBench);
}

TEST_F(FirmwareServiceTest, BoardClass) {
  // Today's firmware declares none: accepted, and said so
  auto r = svc.setTarget(kDevice, "other", false, "x");
  ASSERT_TRUE(r.isSuccess());
  EXPECT_FALSE(r.getData().warning.empty());

  declared.answer.declared->boardClass = kClassL475;
  EXPECT_EQ(svc.setTarget(kDevice, "other", false, "x").getStatusCode(), 409);
  r = svc.setTarget(kDevice, "field", false, "x");
  ASSERT_TRUE(r.isSuccess());
  EXPECT_TRUE(r.getData().warning.empty());

  declared.answer.declared.reset(); // never declared anything
  r = svc.setTarget(kDevice, "other", false, "x");
  ASSERT_TRUE(r.isSuccess());
  EXPECT_FALSE(r.getData().warning.empty());
}

TEST_F(FirmwareServiceTest, FailureToFindOutIsNotNeverDeclared) {
  declared.answer = {.statusCode = 503, .errorMessage = "gateway down"};
  EXPECT_EQ(svc.setTarget(kDevice, "field", false, "x").getStatusCode(), 503);
  EXPECT_EQ(svc.status(kDevice).getStatusCode(), 503);
  EXPECT_EQ(repo.targetsSet, 0);
  // Clearing needs nothing from the station
  EXPECT_TRUE(svc.setTarget(kDevice, std::nullopt, false, "x").isSuccess());
}

TEST_F(FirmwareServiceTest, Retry) {
  EXPECT_EQ(svc.retry("other-device").getStatusCode(), 404);
  EXPECT_EQ(svc.retry(kDevice).getStatusCode(), 409);
  targets("field");
  ASSERT_TRUE(svc.retry(kDevice).isSuccess());
  EXPECT_EQ(svc.retry(kDevice).getData().attempt, 3);
}

TEST_F(FirmwareServiceTest, Permission) {
  EXPECT_FALSE(repo.device->otaEnabled);
  ASSERT_TRUE(svc.setOtaEnabled(kDevice, true, "rafael").isSuccess());
  EXPECT_TRUE(repo.device->otaEnabled);
  EXPECT_EQ(repo.lastBy, "rafael");
  ASSERT_TRUE(svc.setOtaEnabled(kDevice, false, "rafael").isSuccess());
  EXPECT_FALSE(repo.device->otaEnabled);
  EXPECT_EQ(svc.setOtaEnabled("other-device", true, "x").getStatusCode(), 404);
}

TEST_F(FirmwareServiceTest, StateFollowsTheStation) {
  EXPECT_EQ(state(), UpdateState::None);
  targets("field");
  EXPECT_EQ(state(), UpdateState::Disabled);
  repo.device->otaEnabled = true;
  EXPECT_EQ(state(), UpdateState::Pending);
  declared.answer.declared->percent = 40;
  EXPECT_EQ(state(), UpdateState::Downloading);
  declares(repo.images["field"].buildId);
  EXPECT_EQ(state(), UpdateState::Applied);
  repo.device->otaEnabled = false; // it runs it, permission or not
  EXPECT_EQ(state(), UpdateState::Applied);
  targets("bench"); // the target changed
  repo.device->otaEnabled = true;
  EXPECT_EQ(state(), UpdateState::Pending);
  declared.answer.declared.reset();
  EXPECT_EQ(state(), UpdateState::Pending);
}

TEST_F(FirmwareServiceTest, RejectionIsOfThatBuildAndAttempt) {
  targets("field");
  repo.device->otaEnabled = true;
  const auto build = repo.images["field"].buildId;
  auto& rejected = declared.answer.declared->rejected;

  rejected = build + "/1/crash";
  EXPECT_EQ(state(), UpdateState::Rejected);
  repo.device->target->attempt = 2; // the admin asked for another try
  EXPECT_EQ(state(), UpdateState::Pending);
  rejected = build + "/2/hang";
  EXPECT_EQ(state(), UpdateState::Rejected);
  rejected = "ffffffffffffffff/9/crash"; // another build
  EXPECT_EQ(state(), UpdateState::Pending);
  repo.device->target->attempt = 1;
  for (const std::string& suffix : {std::string(), std::string("/"), std::string("/x/crash"),
                                    std::string("/0/crash"), std::string("/-3/crash")}) {
    rejected = build + suffix; // no readable attempt: the first
    EXPECT_EQ(state(), UpdateState::Rejected) << suffix;
  }
  repo.device->otaEnabled = false; // a rejection is worth showing either way
  EXPECT_EQ(state(), UpdateState::Rejected);
}

TEST_F(FirmwareServiceTest, StatusOfAnUnknownDevice) {
  EXPECT_EQ(svc.status("other-device").getStatusCode(), 404);
  const auto r = svc.status(kDevice);
  ASSERT_TRUE(r.isSuccess());
  EXPECT_EQ(r.getData().state, UpdateState::None);
}

TEST(UpdateStateNames, AreStable) {
  EXPECT_EQ(toString(UpdateState::None), "none");
  EXPECT_EQ(toString(UpdateState::Applied), "applied");
  EXPECT_EQ(toString(UpdateState::Rejected), "rejected");
  EXPECT_EQ(toString(UpdateState::Disabled), "disabled");
  EXPECT_EQ(toString(UpdateState::Downloading), "downloading");
  EXPECT_EQ(toString(UpdateState::Pending), "pending");
}
