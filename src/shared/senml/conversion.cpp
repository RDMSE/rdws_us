#include "conversion.h"

#include <cmath>
#include <cstdio>
#include <ctime>

namespace rdws::senml {

namespace {

struct UnitRule {
  const char* senml;
  const char* sensor;
  double factor;
};

// SenML unit registry (RFC 8428 §12.1) -> units in use in sensors.unit (2026-10-05).
constexpr UnitRule kUnitRules[] = {
    {"Cel", "°C", 1.0},   // temperature
    {"%RH", "%", 1.0},    // humidity
    {"/", "%", 100.0},    // moisture: SenML ratio 0..1 -> percent
    {"Pa", "kPa", 0.001}, // pressure
    {"lx", "lux", 1.0},   // luminosity
};

} // namespace

std::optional<double> toSensorUnit(double value, const std::string& senmlUnit,
                                   const std::string& sensorUnit) {
  if (senmlUnit.empty()) {
    return std::nullopt;
  }
  if (senmlUnit == sensorUnit) {
    return value;
  }
  for (const auto& rule : kUnitRules) {
    if (senmlUnit == rule.senml && sensorUnit == rule.sensor) {
      return value * rule.factor;
    }
  }
  return std::nullopt;
}

SenmlValue fromSensorUnit(double value, const std::string& sensorUnit,
                          const std::string& sensorType) {
  if (sensorUnit == "%") {
    if (sensorType == "humidity") {
      return {"%RH", value};
    }
    if (sensorType == "moisture") {
      return {"/", value / 100.0};
    }
    return {sensorUnit, value};
  }
  for (const auto& rule : kUnitRules) {
    if (sensorUnit == rule.sensor) {
      return {rule.senml, value / rule.factor};
    }
  }
  return {sensorUnit, value};
}

std::optional<double> fromIso8601(const std::string& iso) {
  std::tm tm{};
  double seconds = 0;
  char zone = '\0';
  if (std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%lf%c", &tm.tm_year, &tm.tm_mon,
                  &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &seconds, &zone) != 7 ||
      zone != 'Z') {
    return std::nullopt;
  }
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  const double whole = std::floor(seconds);
  tm.tm_sec = static_cast<int>(whole);
  const std::time_t t = timegm(&tm);
  if (t == static_cast<std::time_t>(-1)) {
    return std::nullopt;
  }
  return static_cast<double>(t) + (seconds - whole);
}

std::string toIso8601(double epochSeconds) {
  const double whole = std::floor(epochSeconds);
  auto seconds = static_cast<std::time_t>(whole);
  auto millis = static_cast<int>(std::lround((epochSeconds - whole) * 1000.0));
  if (millis == 1000) {
    ++seconds;
    millis = 0;
  }

  std::tm tm{};
  gmtime_r(&seconds, &tm);
  char buf[32];
  const size_t len = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  std::snprintf(buf + len, sizeof(buf) - len, ".%03dZ", millis);
  return buf;
}

} // namespace rdws::senml
