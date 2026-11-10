// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

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

// What device_service would answer through the gateway (device.diagnostics)
class FakeDeclaredSource : public rdws::device_config::IDeclaredConfigSource {
public:
  rdws::device_config::DeclaredLookup answer;
  rdws::device_config::DeclaredLookup declared(const std::string& /*deviceId*/) override {
    return answer;
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
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);

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
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);

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
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);

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
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);

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
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);

  EXPECT_TRUE(svc.update("9", {.configJson = R"({"sampling_interval_s":30})", .updatedBy = "t"})
                  .isSuccess());
}

TEST(DeviceConfigService, OtherTypes_NotValidated) {
  FakeRepo repo;
  repo.stored = device("single_sensor", false);
  FakeSensorRepo sensors;
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);

  EXPECT_TRUE(svc.update("5", {.configJson = R"({"anything":true})", .updatedBy = "t"})
                  .isSuccess());
}

TEST(DeviceConfigService, WeatherStation_ChipNameNotAccepted) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  FakeSensorRepo sensors;
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);

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
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);

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
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);

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
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);

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
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);

  // a disabled channel keeps its sensor_id for later, so it must be right too
  EXPECT_FALSE(svc.update("12", {.configJson = R"({"onboard":[
                                   {"chan":"press","sensor_id":31,"enabled":false}]})",
                                 .updatedBy = "t"})
                   .isSuccess());
}

namespace {

// A station with the three channels, ready to take triggers through a merge patch
constexpr char kStation[] =
    R"({"agg_window_s":600,"onboard":[{"chan":"temp","sensor_id":31},)"
    R"({"chan":"humidity","sensor_id":32},{"chan":"press","sensor_id":33,"enabled":false}]})";

bool acceptsTriggers(const std::string& triggers, std::string* error = nullptr,
                     const std::string& base = kStation) {
  FakeRepo repo;
  repo.stored = device("weather_station", false, base);
  FakeSensorRepo sensors;
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);
  const auto result =
      svc.update("12", {.configJson = R"({"triggers":)" + triggers + "}", .updatedBy = "t"});
  if (error != nullptr) {
    *error = result.getErrorMessage();
  }
  return result.isSuccess();
}

} // namespace

TEST(DeviceConfigService, Triggers_LevelAndTrendAccepted) {
  EXPECT_TRUE(acceptsTriggers(R"([{"chan":"temp","op":"lt","thr":5.0}])"));
  EXPECT_TRUE(acceptsTriggers(R"([{"chan":"temp","op":"lte","thr":-2.5,"hys":1}])"));
  EXPECT_TRUE(acceptsTriggers(R"([{"chan":"humidity","op":"rise","thr":10,"win_s":3600}])"));
}

TEST(DeviceConfigService, Triggers_ShapeRejected) {
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"temp","op":"eq","thr":5}])"));          // unknown op
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"temp","op":"lt"}])"));                  // no thr
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"temp","op":"lt","thr":5,"hys":-1}])")); // hys < 0
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"wind","op":"lt","thr":5}])"));          // unknown chan
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"temp","op":"lt","thr":5,"x":1}])"));    // unknown key
}

TEST(DeviceConfigService, Triggers_WinSOnlyOnTrendOperators) {
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"temp","op":"fall","thr":2}])"));
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"temp","op":"lt","thr":5,"win_s":600}])"));
}

TEST(DeviceConfigService, Triggers_TrendThresholdMustBePositive) {
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"temp","op":"fall","thr":0,"win_s":600}])"));
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"temp","op":"rise","thr":-1,"win_s":600}])"));
}

TEST(DeviceConfigService, Triggers_AtMostEight) {
  std::string rules = "[";
  for (int i = 0; i < 9; ++i) {
    rules += std::string(i > 0 ? "," : "") + R"({"chan":"temp","op":"lt","thr":5})";
  }
  EXPECT_FALSE(acceptsTriggers(rules + "]"));
}

TEST(DeviceConfigService, Triggers_ChannelMustBeSampled) {
  std::string error;
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"press","op":"lt","thr":90}])", &error)); // disabled
  EXPECT_NE(error.find("triggers[0]"), std::string::npos);
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"temp","op":"lt","thr":5}])", nullptr, "{}"));
}

TEST(DeviceConfigService, Triggers_WinSMustBeMultipleOfTheWindow) {
  EXPECT_TRUE(acceptsTriggers(R"([{"chan":"temp","op":"fall","thr":2,"win_s":10800}])"));
  std::string error;
  EXPECT_FALSE(acceptsTriggers(R"([{"chan":"temp","op":"fall","thr":2,"win_s":900}])", &error));
  EXPECT_NE(error.find("multiple of agg_window_s"), std::string::npos);
}

TEST(DeviceConfigService, Triggers_WindowDefaultsTo600WithoutTheKey) {
  constexpr char kNoWindow[] = R"({"onboard":[{"chan":"temp","sensor_id":31}]})";
  EXPECT_TRUE(
      acceptsTriggers(R"([{"chan":"temp","op":"fall","thr":2,"win_s":1200}])", nullptr, kNoWindow));
  EXPECT_FALSE(
      acceptsTriggers(R"([{"chan":"temp","op":"fall","thr":2,"win_s":300}])", nullptr, kNoWindow));
}

TEST(DeviceConfigService, Triggers_ChangingTheWindowRevalidatesStoredRules) {
  FakeRepo repo;
  repo.stored = device("weather_station", false,
                       R"({"agg_window_s":600,"onboard":[{"chan":"temp","sensor_id":31}],)"
                       R"("triggers":[{"chan":"temp","op":"fall","thr":2,"win_s":1200}]})");
  FakeSensorRepo sensors;
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);
  EXPECT_FALSE(
      svc.update("12", {.configJson = R"({"agg_window_s":900})", .updatedBy = "t"}).isSuccess());
  EXPECT_FALSE(repo.written.has_value());
  EXPECT_TRUE(
      svc.update("12", {.configJson = R"({"agg_window_s":300})", .updatedBy = "t"}).isSuccess());
}

TEST(DeviceConfigService, SimulatedStation_TriggersNotValidated) {
  FakeRepo repo;
  repo.stored = device("weather_station", true);
  FakeSensorRepo sensors;
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);
  EXPECT_TRUE(
      svc.update("12", {.configJson = R"({"triggers":"anything"})", .updatedBy = "t"}).isSuccess());
}

// ─── Config status (GET /devices/{id}/config/status) ─────────────────────────

TEST(ConfigState, FourStates) {
  using rdws::device_config::configState;
  EXPECT_EQ(configState(24, std::nullopt, std::nullopt), "unknown");
  EXPECT_EQ(configState(24, 24, std::nullopt), "applied");
  EXPECT_EQ(configState(24, 23, 24), "rejected");
  EXPECT_EQ(configState(24, 23, std::nullopt), "pending");
}

// cfg_rej names the version it refused: an older refusal doesn't reject the current one.
TEST(ConfigState, OlderRejectionMeansPending) {
  EXPECT_EQ(rdws::device_config::configState(25, 23, 24), "pending");
}

TEST(DeviceConfigService, Status_FromTheLatestDeclaration) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  repo.stored->version = 24;
  FakeSensorRepo sensors;
  FakeDeclaredSource declared;
  declared.answer.declared = rdws::device_config::DeclaredConfig{
      .declared = 23, .rejected = 24, .declaredAt = "2026-11-11T09:00:00Z"};
  DeviceConfigService svc(repo, sensors, declared);

  const auto result = svc.status("12");
  ASSERT_TRUE(result.isSuccess());
  const auto& status = result.getData();
  EXPECT_EQ(status.version, 24);
  EXPECT_EQ(status.declared, 23);
  EXPECT_EQ(status.rejected, 24);
  EXPECT_EQ(status.declaredAt, "2026-11-11T09:00:00Z");
  EXPECT_EQ(status.state, "rejected");
}

TEST(DeviceConfigService, Status_NeverDeclaredIsUnknown) {
  FakeRepo repo;
  repo.stored = device("weather_station", true);
  FakeSensorRepo sensors;
  FakeDeclaredSource declared; // 200, nothing declared
  DeviceConfigService svc(repo, sensors, declared);

  const auto result = svc.status("12");
  ASSERT_TRUE(result.isSuccess());
  EXPECT_FALSE(result.getData().declared.has_value());
  EXPECT_TRUE(result.getData().declaredAt.empty());
  EXPECT_EQ(result.getData().state, "unknown");
}

TEST(DeviceConfigService, Status_UnknownDeviceIs404) {
  FakeRepo repo; // no stored config
  FakeSensorRepo sensors;
  FakeDeclaredSource declared;
  DeviceConfigService svc(repo, sensors, declared);
  const auto result = svc.status("999");
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.getStatusCode(), 404);
}

// Gateway down or timeout must not read as "never declared"
TEST(DeviceConfigService, Status_LookupFailurePassesThrough) {
  FakeRepo repo;
  repo.stored = device("weather_station", false);
  FakeSensorRepo sensors;
  FakeDeclaredSource declared;
  declared.answer.statusCode = 503;
  declared.answer.errorMessage = "Not connected to gateway";
  DeviceConfigService svc(repo, sensors, declared);

  const auto result = svc.status("12");
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.getStatusCode(), 503);
}
