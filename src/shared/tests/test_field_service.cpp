// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "service/FieldService.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using rdws::farm::FarmValidation;
using rdws::farm::IFarmValidator;
using rdws::field::Field;
using rdws::field::FieldCreate;
using rdws::field::FieldService;
using rdws::field::FieldUpdate;
using rdws::field::IFieldRepository;

namespace {

constexpr auto kFarm = "3f2b8c1e-9d4a-4b7e-8a21-0c5d6e7f8a9b";  // internal id 3
constexpr auto kField = "9b8a7f6e-5d4c-4b3a-8291-0f1e2d3c4b5a"; // an existing field
constexpr auto kUnknown = "00000000-0000-4000-8000-000000000000";

// farm_service stand-in: knows kFarm (internal id 3)
class FakeFarmValidator : public IFarmValidator {
public:
  int statusWhenMissing = 404;
  std::vector<std::string> asked;

  FarmValidation resolve(const std::string& farmPublicId) override {
    asked.push_back(farmPublicId);
    if (farmPublicId == kFarm) {
      return {.found = true, .id = "3"};
    }
    return {.found = false, .statusCode = statusWhenMissing, .errorMessage = "gateway down"};
  }
};

class FakeFieldRepo : public IFieldRepository {
public:
  std::optional<FieldCreate> created;
  std::optional<std::string> listedFarm;

  std::vector<Field> findAll(const std::string& farmPublicId) override {
    listedFarm = farmPublicId;
    return {};
  }
  std::optional<Field> findById(const std::string&) override { return std::nullopt; }
  std::optional<Field> findByPublicId(const std::string&) override { return std::nullopt; }
  std::string create(const FieldCreate& data) override {
    created = data;
    return kField;
  }
  bool update(const std::string& publicId, const FieldUpdate&) override {
    return publicId == kField;
  }
  bool remove(const std::string& publicId) override { return publicId == kField; }
};

} // namespace

TEST(FieldService, Create_StoresTheFarmsInternalAndPublicIds) {
  FakeFieldRepo repo;
  FakeFarmValidator farms;
  FieldService svc(repo, farms);
  const auto result = svc.create({.farmPublicId = kFarm, .name = "talhão", .updatedBy = "t"});
  ASSERT_TRUE(result.isSuccess()) << result.getErrorMessage();
  EXPECT_EQ(result.getStatusCode(), 201);
  EXPECT_EQ(result.getData(), kField); // the field's public id
  ASSERT_TRUE(repo.created.has_value());
  EXPECT_EQ(repo.created->farmId, "3"); // resolved: the FK needs the internal id
  EXPECT_EQ(repo.created->farmPublicId, kFarm);
}

TEST(FieldService, Create_UnknownFarmIs404AndNothingWritten) {
  FakeFieldRepo repo;
  FakeFarmValidator farms;
  FieldService svc(repo, farms);
  const auto result = svc.create({.farmPublicId = kUnknown, .name = "x", .updatedBy = "t"});
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.getStatusCode(), 404);
  EXPECT_NE(result.getErrorMessage().find(kUnknown), std::string::npos);
  EXPECT_FALSE(repo.created.has_value());
}

TEST(FieldService, Create_FarmServiceDownKeepsItsStatus) {
  FakeFieldRepo repo;
  FakeFarmValidator farms;
  farms.statusWhenMissing = 503;
  FieldService svc(repo, farms);
  const auto result = svc.create({.farmPublicId = kUnknown, .name = "x", .updatedBy = "t"});
  EXPECT_EQ(result.getStatusCode(), 503); // not misreported as 404
  EXPECT_NE(result.getErrorMessage().find("gateway down"), std::string::npos);
  EXPECT_FALSE(repo.created.has_value());
}

TEST(FieldService, Create_MissingFarmIs400WithoutAskingTheFarmService) {
  FakeFieldRepo repo;
  FakeFarmValidator farms;
  FieldService svc(repo, farms);
  EXPECT_EQ(svc.create({.name = "x", .updatedBy = "t"}).getStatusCode(), 400);
  EXPECT_TRUE(farms.asked.empty());
}

TEST(FieldService, FindAll_FiltersByTheFarmsPublicId) {
  FakeFieldRepo repo;
  FakeFarmValidator farms;
  FieldService svc(repo, farms);
  ASSERT_TRUE(svc.findAll(kFarm).isSuccess());
  EXPECT_EQ(repo.listedFarm, kFarm);
}

TEST(FieldService, FindAll_UnknownFarmIs404NotAnEmptyList) {
  FakeFieldRepo repo;
  FakeFarmValidator farms;
  FieldService svc(repo, farms);
  EXPECT_EQ(svc.findAll(kUnknown).getStatusCode(), 404);
  EXPECT_FALSE(repo.listedFarm.has_value());
}

TEST(FieldService, FindAll_WithoutFarmListsEverythingWithoutAsking) {
  FakeFieldRepo repo;
  FakeFarmValidator farms;
  FieldService svc(repo, farms);
  ASSERT_TRUE(svc.findAll().isSuccess());
  EXPECT_TRUE(farms.asked.empty());
}

TEST(FieldService, UpdateAndRemove_UnknownFieldIsNotFound) {
  FakeFieldRepo repo;
  FakeFarmValidator farms;
  FieldService svc(repo, farms);
  EXPECT_TRUE(svc.update(kField, {.name = "n", .updatedBy = "t"}).isSuccess());
  const auto missing = svc.update(kUnknown, {.name = "n", .updatedBy = "t"});
  EXPECT_FALSE(missing.isSuccess());
  EXPECT_EQ(missing.getStatusCode(), 404);
  EXPECT_TRUE(svc.remove(kField));
  EXPECT_FALSE(svc.remove(kUnknown));
}
