// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "service/FarmService.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using rdws::farm::Farm;
using rdws::farm::FarmCreate;
using rdws::farm::FarmService;
using rdws::farm::FarmUpdate;
using rdws::farm::IFarmRepository;

namespace {

// pg_timezone_names stand-in: knows two names
class FakeFarmRepo : public IFarmRepository {
public:
  std::optional<FarmCreate> created;
  std::optional<FarmUpdate> updated;

  std::vector<Farm> findAll() override { return {}; }
  std::optional<Farm> findById(const std::string&) override { return std::nullopt; }
  std::string create(const FarmCreate& data) override {
    created = data;
    return "7";
  }
  bool update(const std::string&, const FarmUpdate& data) override {
    updated = data;
    return true;
  }
  bool remove(const std::string&) override { return true; }
  bool isKnownTimezone(const std::string& name) override {
    return name == "America/Sao_Paulo" || name == "Europe/Vienna";
  }
};

} // namespace

TEST(FarmService, Create_KnownTimezoneStored) {
  FakeFarmRepo repo;
  FarmService svc(repo);
  const auto result = svc.create({.name = "f", .timezone = "Europe/Vienna", .updatedBy = "t"});
  ASSERT_TRUE(result.isSuccess()) << result.getErrorMessage();
  EXPECT_EQ(result.getData(), "7");
  EXPECT_EQ(result.getStatusCode(), 201);
  EXPECT_EQ(repo.created->timezone, "Europe/Vienna");
}

TEST(FarmService, Create_WithoutTimezoneUsesTheDefault) {
  FakeFarmRepo repo;
  FarmService svc(repo);
  ASSERT_TRUE(svc.create({.name = "f", .updatedBy = "t"}).isSuccess());
  EXPECT_TRUE(repo.created->timezone.empty()); // the column default applies
}

TEST(FarmService, Create_UnknownTimezoneIs400AndNothingWritten) {
  FakeFarmRepo repo;
  FarmService svc(repo);
  const auto result = svc.create({.name = "f", .timezone = "Mars/Olympus", .updatedBy = "t"});
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.getStatusCode(), 400);
  EXPECT_NE(result.getErrorMessage().find("Mars/Olympus"), std::string::npos);
  EXPECT_FALSE(repo.created.has_value());
}

TEST(FarmService, Update_UnknownTimezoneIs400AndNothingWritten) {
  FakeFarmRepo repo;
  FarmService svc(repo);
  const auto result = svc.update("7", {.name = "f", .timezone = "UTC+3", .updatedBy = "t"});
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.getStatusCode(), 400);
  EXPECT_FALSE(repo.updated.has_value());
}

TEST(FarmService, Update_KeepsTimezoneWhenNotGiven) {
  FakeFarmRepo repo;
  FarmService svc(repo);
  ASSERT_TRUE(svc.update("7", {.name = "f", .updatedBy = "t"}).isSuccess());
  EXPECT_TRUE(repo.updated->timezone.empty()); // repository leaves the column as is
}
