#include "service/DeviceConfigService.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>
#include <string>

using rdws::device_config::DeviceConfig;
using rdws::device_config::DeviceConfigService;
using rdws::device_config::DeviceConfigUpdate;
using rdws::device_config::IDeviceConfigRepository;
using rdws::sensor::ISensorRepository;
using rdws::sensor::Sensor;

namespace {

class FakeRepo : public IDeviceConfigRepository {
public:
  std::optional<DeviceConfig> stored;
  std::optional<std::string> written;

  std::optional<DeviceConfig> findByDeviceId(const std::string& /*deviceId*/) override {
    return stored;
  }
  bool update(const std::string& /*deviceId*/, const DeviceConfigUpdate& data) override {
    written = data.configJson;
    return true;
  }
};

// Sensors of device 12 as in rdws_qa: 31 temperature, 32 humidity, 33 pressure
class FakeSensorRepo : public ISensorRepository {
public:
  std::vector<Sensor> findAll(const std::string& deviceId) override {
    if (deviceId != "12") {
      return {};
    }
    return {sensor("31", "temperature"), sensor("32", "humidity"), sensor("33", "pressure")};
  }
  std::optional<Sensor> findById(const std::string&) override { return std::nullopt; }
  std::string create(const rdws::sensor::SensorCreate&) override { return {}; }
  bool update(const std::string&, const rdws::sensor::SensorUpdate&) override { return false; }
  bool remove(const std::string&) override { return false; }

private:
  static Sensor sensor(const std::string& id, const std::string& type) {
    Sensor s;
    s.id = id;
    s.deviceId = "12";
    s.type = type;
    return s;
  }
};

DeviceConfig device(const std::string& type, bool simulated, const std::string& config = "{}") {
  DeviceConfig c;
  c.deviceId = "12";
  c.config = config;
  c.version = 1;
  c.deviceType = type;
  c.isSimulated = simulated;
  return c;
}

} // namespace

TEST(DeviceConfigService, WeatherStation_ValidConfigIsStored) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  FakeSensorRepo sensors;
  DeviceConfigService svc(repo, sensors);

  const auto result = svc.update(
      "12", {.configJson = R"({"agg_window_s":600,"onboard":[
               {"chan":"temp","sensor_id":31}]})",
             .updatedBy = "test"});

  EXPECT_TRUE(result.isSuccess()) << result.getErrorMessage();
  ASSERT_TRUE(repo.written);
}

TEST(DeviceConfigService, WeatherStation_UnknownKeyRejected) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  FakeSensorRepo sensors;
  DeviceConfigService svc(repo, sensors);

  const auto result =
      svc.update("12", {.configJson = R"({"agg_windows":600})", .updatedBy = "test"});

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.getStatusCode(), 400);
  EXPECT_FALSE(repo.written); // nothing stored
}

TEST(DeviceConfigService, WeatherStation_ValidatesTheMergedResult) {
  FakeRepo repo;
  // Already invalid in the DB (e.g. from before V12): a patch that doesn't touch the bad
  // key still yields an invalid merged config, so it's rejected.
  repo.stored = device("weather_station", false, R"({"bogus":1})");
  FakeSensorRepo sensors;
  DeviceConfigService svc(repo, sensors);

  EXPECT_FALSE(svc.update("12", {.configJson = R"({"agg_window_s":600})", .updatedBy = "t"})
                   .isSuccess());
  // Removing it with a merge-patch null fixes it.
  EXPECT_TRUE(svc.update("12", {.configJson = R"({"bogus":null})", .updatedBy = "t"})
                  .isSuccess());
}

TEST(DeviceConfigService, WeatherStation_OutOfRangeRejected) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  FakeSensorRepo sensors;
  DeviceConfigService svc(repo, sensors);

  EXPECT_FALSE(svc.update("12", {.configJson = R"({"agg_window_s":5})", .updatedBy = "t"})
                   .isSuccess());
  EXPECT_FALSE(svc.update("12", {.configJson = R"({"onboard":[{"chan":"temp","sensor_id":0}]})",
                                 .updatedBy = "t"})
                   .isSuccess());
}

TEST(DeviceConfigService, SimulatedWeatherStation_NotValidated) {
  FakeRepo repo;
  repo.stored = device("weather_station", true);
  FakeSensorRepo sensors;
  DeviceConfigService svc(repo, sensors);

  EXPECT_TRUE(svc.update("9", {.configJson = R"({"sampling_interval_s":30})", .updatedBy = "t"})
                  .isSuccess());
}

TEST(DeviceConfigService, OtherTypes_NotValidated) {
  FakeRepo repo;
  repo.stored = device("single_sensor", false);
  FakeSensorRepo sensors;
  DeviceConfigService svc(repo, sensors);

  EXPECT_TRUE(svc.update("5", {.configJson = R"({"anything":true})", .updatedBy = "t"})
                  .isSuccess());
}

TEST(DeviceConfigService, WeatherStation_ChipNameNotAccepted) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  FakeSensorRepo sensors;
  DeviceConfigService svc(repo, sensors);

  // onboard[] names the quantity only; the chip is a firmware detail
  EXPECT_FALSE(svc.update("12", {.configJson = R"({"onboard":[{"source":"hts221",
                                   "chan":"temp","sensor_id":31}]})",
                                 .updatedBy = "t"})
                   .isSuccess());
}

TEST(DeviceConfigService, WeatherStation_EnabledFlag) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  FakeSensorRepo sensors;
  DeviceConfigService svc(repo, sensors);

  EXPECT_TRUE(svc.update("12", {.configJson = R"({"onboard":[
                                  {"chan":"temp","sensor_id":31,"enabled":false},
                                  {"chan":"press","sensor_id":33}]})",
                                .updatedBy = "t"})
                  .isSuccess());
  EXPECT_FALSE(svc.update("12", {.configJson = R"({"onboard":[
                                   {"chan":"temp","sensor_id":31,"enabled":"no"}]})",
                                 .updatedBy = "t"})
                   .isSuccess());
}

TEST(DeviceConfigService, WeatherStation_SwappedSensorIdsRejected) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  FakeSensorRepo sensors;
  DeviceConfigService svc(repo, sensors);

  // press -> 32 (humidity) and humidity -> 33 (pressure): the mix-up seen in QA
  const auto result = svc.update("12", {.configJson = R"({"onboard":[
                                          {"chan":"temp","sensor_id":31},
                                          {"chan":"press","sensor_id":32},
                                          {"chan":"humidity","sensor_id":33}]})",
                                        .updatedBy = "t"});
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.getStatusCode(), 400);
  EXPECT_NE(result.getErrorMessage().find("sensor 32 is humidity, chan press needs pressure"),
            std::string::npos)
      << result.getErrorMessage();
  EXPECT_FALSE(repo.written);
}

TEST(DeviceConfigService, WeatherStation_SensorOfAnotherDeviceRejected) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  FakeSensorRepo sensors;
  DeviceConfigService svc(repo, sensors);

  const auto result = svc.update(
      "12", {.configJson = R"({"onboard":[{"chan":"temp","sensor_id":12}]})", .updatedBy = "t"});
  EXPECT_FALSE(result.isSuccess());
  EXPECT_NE(result.getErrorMessage().find("sensor 12 is not a sensor of device 12"),
            std::string::npos)
      << result.getErrorMessage();
}

TEST(DeviceConfigService, WeatherStation_DisabledChannelStillChecked) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  FakeSensorRepo sensors;
  DeviceConfigService svc(repo, sensors);

  // a disabled channel keeps its sensor_id for later, so it must be right too
  EXPECT_FALSE(svc.update("12", {.configJson = R"({"onboard":[
                                   {"chan":"press","sensor_id":31,"enabled":false}]})",
                                 .updatedBy = "t"})
                   .isSuccess());
}
