#pragma once

// rdws-specific glue between parsed SenML records and the sensor_readings model
// (Plano_Telemetria.md, D3/DP1/DP3). Pure functions, no I/O.

#include <cstdint>
#include <optional>
#include <string>

namespace rdws::senml {

// Per-reading flags (fl_), same bits as the firmware's RECORD_FLAG_* (DP3).
inline constexpr uint16_t kFlagTimeUnsynced = 0x1;
inline constexpr uint16_t kFlagTrigger = 0x2;
inline constexpr uint16_t kFlagPartialWindow = 0x4;
inline constexpr uint16_t kKnownFlags = kFlagTimeUnsynced | kFlagTrigger | kFlagPartialWindow;

// Converts `value` from a SenML unit to the unit registered in sensors.unit (DP1: the server
// converts, the DB keeps its own naming). Identical units always convert 1:1. Returns nullopt
// when there's no rule for the pair, including a missing SenML unit.
[[nodiscard]] std::optional<double> toSensorUnit(double value, const std::string& senmlUnit,
                                                 const std::string& sensorUnit);

// Epoch seconds -> "YYYY-MM-DDTHH:MM:SS.mmmZ", the timestamp format of the sensor_readings
// queue message.
[[nodiscard]] std::string toIso8601(double epochSeconds);

} // namespace rdws::senml
