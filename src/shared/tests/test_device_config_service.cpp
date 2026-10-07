#include "service/DeviceConfigService.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

using rdws::device_config::DeviceConfig;
using rdws::device_config::DeviceConfigService;
using rdws::device_config::DeviceConfigUpdate;
using rdws::device_config::IDeviceConfigRepository;

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
  DeviceConfigService svc(repo);

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
  DeviceConfigService svc(repo);

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
  DeviceConfigService svc(repo);

  EXPECT_FALSE(svc.update("12", {.configJson = R"({"agg_window_s":600})", .updatedBy = "t"})
                   .isSuccess());
  // Removing it with a merge-patch null fixes it.
  EXPECT_TRUE(svc.update("12", {.configJson = R"({"bogus":null})", .updatedBy = "t"})
                  .isSuccess());
}

TEST(DeviceConfigService, WeatherStation_OutOfRangeRejected) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  DeviceConfigService svc(repo);

  EXPECT_FALSE(svc.update("12", {.configJson = R"({"agg_window_s":5})", .updatedBy = "t"})
                   .isSuccess());
  EXPECT_FALSE(svc.update("12", {.configJson = R"({"onboard":[{"chan":"temp","sensor_id":0}]})",
                                 .updatedBy = "t"})
                   .isSuccess());
}

TEST(DeviceConfigService, SimulatedWeatherStation_NotValidated) {
  FakeRepo repo;
  repo.stored = device("weather_station", true);
  DeviceConfigService svc(repo);

  EXPECT_TRUE(svc.update("9", {.configJson = R"({"sampling_interval_s":30})", .updatedBy = "t"})
                  .isSuccess());
}

TEST(DeviceConfigService, OtherTypes_NotValidated) {
  FakeRepo repo;
  repo.stored = device("single_sensor", false);
  DeviceConfigService svc(repo);

  EXPECT_TRUE(svc.update("5", {.configJson = R"({"anything":true})", .updatedBy = "t"})
                  .isSuccess());
}

TEST(DeviceConfigService, WeatherStation_ChipNameNotAccepted) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  DeviceConfigService svc(repo);

  // onboard[] names the quantity only; the chip is a firmware detail
  EXPECT_FALSE(svc.update("12", {.configJson = R"({"onboard":[{"source":"hts221",
                                   "chan":"temp","sensor_id":31}]})",
                                 .updatedBy = "t"})
                   .isSuccess());
}
