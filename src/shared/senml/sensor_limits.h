// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Plausible ranges of a reading, by sensors.unit (Plano_Ingestion.md, "Validação contra faixas
// plausíveis"). Physical limits, not agronomic ones: a value outside can't be a correct
// measurement (a wet HTS221 read 131.5 % RH on 2026-11-09). The IngestionService still stores
// it, with kFlagOutOfRange set, so the evidence of a faulty sensor isn't lost; indices, alarms
// and dashboards ignore flagged readings. Keyed by unit, not sensor type, because the
// ingestion's owner cache carries the unit: "%" covers humidity and soil moisture alike.

#include <cstdint>
#include <string>

namespace rdws::senml {

// Set by the server in sensor_readings.flags (SMALLINT). Bit 14, the highest that keeps the
// column positive, far from the firmware's fl_ bits, which grow from bit 0 (conversion.h).
inline constexpr uint16_t kFlagOutOfRange = 0x4000;

struct SensorLimits {
  const char* unit; // sensors.unit
  double min;
  double max;
};

inline constexpr SensorLimits kSensorLimits[] = {
    {"°C", -80.0, 70.0},    // air temperature: beyond the records on Earth's surface
    {"%", 0.0, 100.0},      // relative humidity, soil moisture
    {"kPa", 30.0, 110.0},   // station pressure: up to ~9000 m altitude
    {"lux", 0.0, 200000.0}, // full sunlight is ~120 000 lx
    {"pH", 0.0, 14.0},
    {"ppm", 0.0, 50000.0},  // CO2
};

// False when `value` is outside the range of `unit`. A unit without limits is always plausible.
[[nodiscard]] inline bool isPlausible(double value, const std::string& unit) {
  for (const auto& limits : kSensorLimits) {
    if (unit == limits.unit) {
      return value >= limits.min && value <= limits.max;
    }
  }
  return true;
}

// Plausible instants of a record (Plano_Telemetria.md, "Instantes impossíveis"). A record outside
// them is dropped, not stored with a flag: its time can't be corrected afterwards, and once
// stored it lands in every time series at the wrong place. The cases seen: an RTC at its reset
// value (an STM32 without backup battery restarts at 2000-01-01, an absolute SenML time) and a
// counter from power-on, which RFC 8428 reads as relative to now (bt: 18000 → now + 5 h).
//   - not before 2020-01-01: older than any device of this platform;
//   - at most 1 h ahead of the server's clock: room for clock drift and packs in transit.
inline constexpr double kEarliestTime = 1577836800.0; // 2020-01-01T00:00:00Z
inline constexpr double kMaxAheadSeconds = 3600.0;

// False when `time` (epoch seconds, as resolved by parse()) is outside those bounds, `now` being
// the server's clock in epoch seconds.
[[nodiscard]] inline bool isPlausibleTime(double time, double now) {
  return time >= kEarliestTime && time <= now + kMaxAheadSeconds;
}

} // namespace rdws::senml
