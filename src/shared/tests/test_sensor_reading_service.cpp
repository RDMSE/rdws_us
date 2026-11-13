// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "service/SensorReadingService.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using rdws::sensor::ISensorValidator;
using rdws::sensor::SensorValidation;
using rdws::sensor_reading::ISensorReadingRepository;
using rdws::sensor_reading::SensorReading;
using rdws::sensor_reading::SensorReadingService;

namespace {

constexpr auto kSensor31 = "5f745c54-0b97-46ca-94ae-97b08aeaf2c9"; // internal id 31
constexpr auto kSensor32 = "7e09d17c-88e2-4592-919f-4eeb659e6577"; // internal id 32
constexpr auto kUnknown = "00000000-0000-4000-8000-000000000000";

// sensor_service stand-in: knows sensors 31 and 32
class FakeSensorValidator : public ISensorValidator {
public:
  int statusWhenMissing = 404;

  SensorValidation resolve(const std::string& sensorPublicId) override {
    if (sensorPublicId == kSensor31) {
      return {.found = true, .id = "31"};
    }
    if (sensorPublicId == kSensor32) {
      return {.found = true, .id = "32"};
    }
    return {.found = false, .statusCode = statusWhenMissing, .errorMessage = "gateway down"};
  }
};

// Reading 1000 belongs to sensor 31
class FakeReadingRepo : public ISensorReadingRepository {
public:
  std::optional<std::string> listedSensor;

  std::vector<SensorReading> findBySensorId(const std::string& sensorId, const std::string&,
                                            const std::string&) override {
    listedSensor = sensorId;
    return {};
  }
  std::optional<SensorReading> findInSensor(const std::string& sensorId,
                                            const std::string& id) override {
    if (sensorId == "31" && id == "1000") {
      return SensorReading{.id = "1000", .sensorId = "31", .value = "21.5"};
    }
    return std::nullopt;
  }
  bool insert(const std::string&, const std::string&, const std::string&, int) override {
    return true;
  }
};

struct Fixture {
  FakeReadingRepo repo;
  FakeSensorValidator sensors;
  SensorReadingService svc{repo, sensors};
};

} // namespace

TEST(SensorReadingService, FindBySensor_ListsByTheInternalId) {
  Fixture f;
  ASSERT_TRUE(f.svc.findBySensor(kSensor31).isSuccess());
  EXPECT_EQ(f.repo.listedSensor, "31"); // sensor_readings is keyed by the internal id
}

TEST(SensorReadingService, FindBySensor_UnknownSensorIs404NotAnEmptyList) {
  Fixture f;
  const auto result = f.svc.findBySensor(kUnknown);
  EXPECT_EQ(result.getStatusCode(), 404);
  EXPECT_NE(result.getErrorMessage().find(kUnknown), std::string::npos);
  EXPECT_FALSE(f.repo.listedSensor.has_value());
}

TEST(SensorReadingService, FindBySensor_SensorServiceDownKeepsItsStatus) {
  Fixture f;
  f.sensors.statusWhenMissing = 503;
  EXPECT_EQ(f.svc.findBySensor(kUnknown).getStatusCode(), 503);
}

TEST(SensorReadingService, FindInSensor_ReadingOfThatSensor) {
  Fixture f;
  const auto result = f.svc.findInSensor(kSensor31, "1000");
  ASSERT_TRUE(result.isSuccess()) << result.getErrorMessage();
  EXPECT_EQ(result.getData().value, "21.5");
}

TEST(SensorReadingService, FindInSensor_ReadingOfAnotherSensorIs404) {
  Fixture f;
  // Reading 1000 is sensor 31's: under sensor 32's path it doesn't exist (the IDOR before)
  EXPECT_EQ(f.svc.findInSensor(kSensor32, "1000").getStatusCode(), 404);
}

TEST(SensorReadingService, FindInSensor_UnknownSensorIs404) {
  Fixture f;
  EXPECT_EQ(f.svc.findInSensor(kUnknown, "1000").getStatusCode(), 404);
}
