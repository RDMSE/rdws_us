// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "trigger_config.h"

#include "conversion.h"

#include "../utils/json_helper.h"

#include <rapidjson/document.h>

#include <cmath>

namespace rdws::senml {

namespace {

struct ChannelFormat {
  const char* chan;
  const char* sensorType;
  const char* senmlUnit;
  double fixedPointScale;
};

constexpr ChannelFormat kChannels[] = {
    {"temp", "temperature", "Cel", 100.0},
    {"humidity", "humidity", "%RH", 100.0},
    {"press", "pressure", "Pa", 1.0},
};

// Replaces `field` of `rule`, if present, by its fixed point. False when it doesn't convert.
bool convertField(rapidjson::Value& rule, const char* field, const std::string& chan,
                  const std::string& sensorUnit) {
  const auto member = rule.FindMember(field);
  if (member == rule.MemberEnd()) {
    return true;
  }
  if (!member->value.IsNumber()) {
    return false;
  }
  const auto fixed = toFirmwareFixedPoint(member->value.GetDouble(), chan, sensorUnit);
  if (!fixed) {
    return false;
  }
  member->value.SetInt64(*fixed);
  return true;
}

} // namespace

std::optional<long long> toFirmwareFixedPoint(double value, const std::string& chan,
                                              const std::string& sensorUnit) {
  for (const auto& channel : kChannels) {
    if (chan == channel.chan) {
      const auto senml = fromSensorUnit(value, sensorUnit, channel.sensorType);
      if (senml.unit != channel.senmlUnit) {
        return std::nullopt;
      }
      return std::llround(senml.value * channel.fixedPointScale);
    }
  }
  return std::nullopt;
}

std::optional<std::string>
configForFirmware(const std::string& configJson,
                  const std::unordered_map<std::string, std::string>& unitBySensor) {
  rapidjson::Document config;
  if (config.Parse(configJson.c_str()).HasParseError() || !config.IsObject()) {
    return std::nullopt;
  }

  const auto triggers = config.FindMember("triggers");
  if (triggers == config.MemberEnd()) {
    return configJson;
  }
  const auto onboard = config.FindMember("onboard");
  if (!triggers->value.IsArray() || onboard == config.MemberEnd() || !onboard->value.IsArray()) {
    return std::nullopt;
  }

  std::unordered_map<std::string, std::string> unitByChan;
  for (const auto& entry : onboard->value.GetArray()) {
    const auto chan = utils::json::getString(entry, "chan");
    const auto sensorId = utils::json::getInt64(entry, "sensor_id");
    if (!chan || !sensorId) {
      return std::nullopt;
    }
    if (const auto unit = unitBySensor.find(std::to_string(*sensorId));
        unit != unitBySensor.end()) {
      unitByChan.emplace(*chan, unit->second);
    }
  }

  for (auto& rule : triggers->value.GetArray()) {
    const auto chan = utils::json::getString(rule, "chan");
    if (!chan) {
      return std::nullopt;
    }
    const auto unit = unitByChan.find(*chan);
    if (unit == unitByChan.end() || !convertField(rule, "thr", *chan, unit->second) ||
        !convertField(rule, "hys", *chan, unit->second)) {
      return std::nullopt;
    }
  }
  return utils::json::docToString(config);
}

} // namespace rdws::senml
