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
