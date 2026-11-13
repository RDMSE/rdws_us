// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "service/SensorService.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using rdws::device::DeviceValidation;
using rdws::device::IDeviceValidator;
using rdws::sensor::ISensorRepository;
using rdws::sensor::Sensor;
using rdws::sensor::SensorCreate;
using rdws::sensor::SensorService;
using rdws::sensor::SensorUpdate;

namespace {

constexpr auto kDevice = "4884a5f7-0e3a-4976-a1d6-662908f185fa"; // internal id 12
constexpr auto kSensor = "9b8a7f6e-5d4c-4b3a-8291-0f1e2d3c4b5a";
constexpr auto kUnknown = "00000000-0000-4000-8000-000000000000";

// device_service stand-in: knows kDevice (internal id 12)
class FakeDeviceValidator : public IDeviceValidator {
public:
  int statusWhenMissing = 404;
  std::vector<std::string> asked;

  DeviceValidation resolve(const std::string& devicePublicId) override {
    asked.push_back(devicePublicId);
    if (devicePublicId == kDevice) {
      return {.found = true, .id = "12"};
    }
    return {.found = false, .statusCode = statusWhenMissing, .errorMessage = "gateway down"};
  }
};

class FakeSensorRepo : public ISensorRepository {
public:
  std::optional<SensorCreate> created;
  std::optional<std::string> listedDevice;

  std::vector<Sensor> findAll(const std::string& devicePublicId) override {
    listedDevice = devicePublicId;
    return {};
  }
  std::optional<Sensor> findById(const std::string&) override { return std::nullopt; }
  std::optional<Sensor> findByPublicId(const std::string&) override { return std::nullopt; }
  std::string create(const SensorCreate& data) override {
    created = data;
    return kSensor;
  }
  bool update(const std::string& publicId, const SensorUpdate&) override {
    return publicId == kSensor;
  }
  bool remove(const std::string& publicId) override { return publicId == kSensor; }
};

struct Fixture {
  FakeSensorRepo repo;
  FakeDeviceValidator devices;
  SensorService svc{repo, devices};
};

} // namespace

TEST(SensorService, Create_StoresTheDevicesInternalAndPublicIds) {
  Fixture f;
  const auto result =
      f.svc.create({.devicePublicId = kDevice, .type = "temperature", .unit = "C", .updatedBy = "t"});
  ASSERT_TRUE(result.isSuccess()) << result.getErrorMessage();
  EXPECT_EQ(result.getStatusCode(), 201);
  EXPECT_EQ(result.getData(), kSensor); // the sensor's public id
  ASSERT_TRUE(f.repo.created.has_value());
  EXPECT_EQ(f.repo.created->deviceId, "12"); // resolved: the FK needs the internal id
  EXPECT_EQ(f.repo.created->devicePublicId, kDevice);
}

TEST(SensorService, Create_UnknownDeviceIs404AndNothingWritten) {
  Fixture f;
  const auto result =
      f.svc.create({.devicePublicId = kUnknown, .type = "temperature", .unit = "C", .updatedBy = "t"});
  EXPECT_EQ(result.getStatusCode(), 404); // before: no check, the FK failed with a 500
  EXPECT_NE(result.getErrorMessage().find(kUnknown), std::string::npos);
  EXPECT_FALSE(f.repo.created.has_value());
}

TEST(SensorService, Create_DeviceServiceDownKeepsItsStatus) {
  Fixture f;
  f.devices.statusWhenMissing = 503;
  const auto result =
      f.svc.create({.devicePublicId = kUnknown, .type = "temperature", .unit = "C", .updatedBy = "t"});
  EXPECT_EQ(result.getStatusCode(), 503);
  EXPECT_FALSE(f.repo.created.has_value());
}

TEST(SensorService, Create_MissingDeviceIs400WithoutAskingTheDeviceService) {
  Fixture f;
  EXPECT_EQ(f.svc.create({.type = "temperature", .unit = "C", .updatedBy = "t"}).getStatusCode(),
            400);
  EXPECT_TRUE(f.devices.asked.empty());
}

TEST(SensorService, FindAll_FiltersByTheDevicesPublicIdAndRejectsAnUnknownDevice) {
  Fixture f;
  ASSERT_TRUE(f.svc.findAll(kDevice).isSuccess());
  EXPECT_EQ(f.repo.listedDevice, kDevice);

  Fixture g;
  EXPECT_EQ(g.svc.findAll(kUnknown).getStatusCode(), 404); // not an empty list
  EXPECT_FALSE(g.repo.listedDevice.has_value());
}

TEST(SensorService, ListOwners_ListsEverySensorWithoutAskingTheDeviceService) {
  Fixture f;
  (void)f.svc.listOwners();
  EXPECT_EQ(f.repo.listedDevice, ""); // no filter
  EXPECT_TRUE(f.devices.asked.empty());
}

TEST(SensorService, UpdateAndRemove_UnknownSensorIsNotFound) {
  Fixture f;
  EXPECT_TRUE(f.svc.update(kSensor, {.type = "temperature", .unit = "C"}).isSuccess());
  EXPECT_EQ(f.svc.update(kUnknown, {.type = "temperature", .unit = "C"}).getStatusCode(), 404);
  EXPECT_TRUE(f.svc.remove(kSensor));
  EXPECT_FALSE(f.svc.remove(kUnknown));
}
