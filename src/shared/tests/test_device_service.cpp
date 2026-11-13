// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "service/DeviceService.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using rdws::device::Device;
using rdws::device::DeviceCreate;
using rdws::device::DeviceIds;
using rdws::device::DeviceService;
using rdws::device::DeviceUpdate;
using rdws::device::IDeviceRepository;
using rdws::device::SimulatedSensor;
using rdws::device_telemetry::DiagnosticsSnapshot;
using rdws::device_telemetry::IDeviceTelemetryRepository;
using rdws::field::FieldValidation;
using rdws::field::IFieldValidator;

namespace {

constexpr auto kField = "3f2b8c1e-9d4a-4b7e-8a21-0c5d6e7f8a9b";  // internal id 5
constexpr auto kDevice = "9b8a7f6e-5d4c-4b3a-8291-0f1e2d3c4b5a"; // internal id 12
constexpr auto kUnknown = "00000000-0000-4000-8000-000000000000";

// field_service stand-in: knows kField (internal id 5)
class FakeFieldValidator : public IFieldValidator {
public:
  int statusWhenMissing = 404;
  std::vector<std::string> asked;

  FieldValidation resolve(const std::string& fieldPublicId) override {
    asked.push_back(fieldPublicId);
    if (fieldPublicId == kField) {
      return {.found = true, .id = "5"};
    }
    return {.found = false, .statusCode = statusWhenMissing, .errorMessage = "gateway down"};
  }
};

class FakeDeviceRepo : public IDeviceRepository {
public:
  std::optional<DeviceCreate> created;
  std::optional<std::string> listedField;

  std::vector<Device> findAll(const std::string& fieldPublicId) override {
    listedField = fieldPublicId;
    return {};
  }
  std::optional<Device> findById(const std::string&) override { return std::nullopt; }
  std::optional<Device> findByPublicId(const std::string&) override { return std::nullopt; }
  DeviceIds create(const DeviceCreate& data) override {
    created = data;
    return {.id = "12", .publicId = kDevice};
  }
  bool update(const std::string& publicId, const DeviceUpdate&) override {
    return publicId == kDevice;
  }
  bool updateLocation(const std::string&, const std::string&) override { return true; }
  bool remove(const std::string& publicId) override { return publicId == kDevice; }
  std::vector<SimulatedSensor> findSimulatedSensorsByDeviceId(const std::string&) override {
    return {};
  }
};

class FakeTelemetry : public IDeviceTelemetryRepository {
public:
  std::optional<std::string> askedFor;

  bool upsert(const std::string&, const std::string&, const std::string&) override { return true; }
  bool upsertConfigSpan(const std::string&, int, const std::string&, const std::string&) override {
    return true;
  }
  bool deleteOlderThan(int) override { return true; }
  std::optional<DiagnosticsSnapshot> latestDiagnostics(const std::string& deviceId) override {
    askedFor = deviceId;
    return DiagnosticsSnapshot{.timestamp = "2026-11-12T10:00:00Z", .dataJson = "{}"};
  }
};

struct Fixture {
  FakeDeviceRepo repo;
  FakeFieldValidator fields;
  FakeTelemetry telemetry;
  DeviceService svc{repo, fields, telemetry};
};

} // namespace

TEST(DeviceService, Create_StoresTheFieldsInternalAndPublicIdsAndReturnsBothDeviceIds) {
  Fixture f;
  const auto result = f.svc.create({.fieldPublicId = kField, .type = "other", .updatedBy = "t"});
  ASSERT_TRUE(result.isSuccess()) << result.getErrorMessage();
  EXPECT_EQ(result.getStatusCode(), 201);
  EXPECT_EQ(result.getData().id, "12");         // the credential is provisioned against it
  EXPECT_EQ(result.getData().publicId, kDevice); // the API answers with it
  ASSERT_TRUE(f.repo.created.has_value());
  EXPECT_EQ(f.repo.created->fieldId, "5"); // resolved: the FK needs the internal id
  EXPECT_EQ(f.repo.created->fieldPublicId, kField);
}

TEST(DeviceService, Create_UnknownFieldIs404AndNothingWritten) {
  Fixture f;
  const auto result = f.svc.create({.fieldPublicId = kUnknown, .type = "other", .updatedBy = "t"});
  EXPECT_EQ(result.getStatusCode(), 404);
  EXPECT_NE(result.getErrorMessage().find(kUnknown), std::string::npos);
  EXPECT_FALSE(f.repo.created.has_value());
}

TEST(DeviceService, Create_FieldServiceDownKeepsItsStatus) {
  Fixture f;
  f.fields.statusWhenMissing = 503;
  const auto result = f.svc.create({.fieldPublicId = kUnknown, .type = "other", .updatedBy = "t"});
  EXPECT_EQ(result.getStatusCode(), 503); // not misreported as 404
  EXPECT_FALSE(f.repo.created.has_value());
}

TEST(DeviceService, Create_MissingFieldIs400WithoutAskingTheFieldService) {
  Fixture f;
  EXPECT_EQ(f.svc.create({.type = "other", .updatedBy = "t"}).getStatusCode(), 400);
  EXPECT_TRUE(f.fields.asked.empty());
}

TEST(DeviceService, FindAll_FiltersByTheFieldsPublicIdAndRejectsAnUnknownField) {
  Fixture f;
  ASSERT_TRUE(f.svc.findAll(kField).isSuccess());
  EXPECT_EQ(f.repo.listedField, kField);

  Fixture g;
  EXPECT_EQ(g.svc.findAll(kUnknown).getStatusCode(), 404); // not an empty list
  EXPECT_FALSE(g.repo.listedField.has_value());
}

TEST(DeviceService, UpdateAndRemove_UnknownDeviceIs404) {
  Fixture f;
  EXPECT_TRUE(f.svc.update(kDevice, {.type = "other", .status = "active"}).isSuccess());
  EXPECT_EQ(f.svc.update(kUnknown, {.type = "other", .status = "active"}).getStatusCode(), 404);
  EXPECT_TRUE(f.svc.remove(kDevice).isSuccess());
  EXPECT_EQ(f.svc.remove(kUnknown).getStatusCode(), 404);
}

TEST(DeviceService, Diagnostics_ReadsTelemetryByTheInternalId) {
  Fixture f;
  const Device device{.id = "12", .publicId = kDevice};
  ASSERT_TRUE(f.svc.diagnostics(device).has_value());
  EXPECT_EQ(f.telemetry.askedFor, "12"); // device_telemetry is keyed by the internal id
}
