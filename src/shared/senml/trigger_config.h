// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Edge trigger rules on their way to a station (Plano_Firmware_WeatherNode.md §4). The stored
// config has `thr` and `hys` in the unit of the channel's sensor (sensors.unit); the firmware
// compares in fixed point: hundredths of Cel and %RH, whole Pa. Pure functions, no I/O.

#include <optional>
#include <string>
#include <unordered_map>

namespace rdws::senml {

// `value` in `sensorUnit` -> the firmware's fixed point for channel `chan`. nullopt when the
// unit can't be converted to the channel's SenML unit, or `chan` is unknown.
[[nodiscard]] std::optional<long long> toFirmwareFixedPoint(double value, const std::string& chan,
                                                            const std::string& sensorUnit);

// The config JSON with every trigger's `thr`/`hys` in fixed point (a config without triggers
// comes back as is). `unitBySensor`: sensors.unit by sensor_id, to go from a rule's `chan`
// through onboard[]. nullopt when any rule can't be converted: the whole config must then
// stay undelivered, since a raw 5.0 read as fixed point means 0.05 °C.
[[nodiscard]] std::optional<std::string>
configForFirmware(const std::string& configJson,
                  const std::unordered_map<std::string, std::string>& unitBySensor);

} // namespace rdws::senml
