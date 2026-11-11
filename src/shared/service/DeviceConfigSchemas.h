// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace rdws::device_config {

// Config of a real weather station (rdws_weather_node), validated by DeviceConfigService on
// the merged config — device_config.update is a merge patch, so only the result has the full
// shape. Strict (additionalProperties: false) to catch typos the firmware would silently
// ignore; nothing is required, since a device starts with "{}" and the firmware falls back to
// its build-time defaults for whatever is missing. Covers only what the firmware applies
// today (Plano_Firmware_WeatherNode.md §5, step 4); pulse/modbus/sdi12 join with steps 5–7.
// onboard[] names the quantity (`chan`), not the chip measuring it: which part provides a
// channel is a board detail the firmware knows (decided 2026-10-07). A channel absent from
// onboard[] isn't sampled; `enabled` (optional, default true) turns a listed one off while
// keeping its sensor_id.
// triggers[] are the station's edge triggers (Plano_Firmware_WeatherNode.md §4). `thr` and `hys`
// are written in the unit of the channel's sensor (sensors.unit), so 5 °C is 5.0; the firmware's
// fixed point is produced when the config is delivered. Rules depending on the rest of the
// config (chan in onboard[], win_s against agg_window_s) are in DeviceConfigService.
inline const std::string WEATHER_STATION_CONFIG_SCHEMA = R"({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "weather_station config",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "transmissions_per_day": { "type": "integer", "minimum": 1, "maximum": 1440 },
    "agg_window_s": { "type": "integer", "minimum": 60, "maximum": 3600 },
    "onboard": {
      "type": "array",
      "maxItems": 3,
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["chan", "sensor_id"],
        "properties": {
          "chan": { "enum": ["temp", "humidity", "press"] },
          "enabled": { "type": "boolean" },
          "sensor_id": { "type": "integer", "minimum": 1, "maximum": 4294967295 }
        }
      }
    },
    "triggers": {
      "type": "array",
      "maxItems": 8,
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["chan", "op", "thr"],
        "properties": {
          "chan": { "enum": ["temp", "humidity", "press"] },
          "op": { "enum": ["lt", "lte", "gt", "gte", "fall", "rise"] },
          "thr": { "type": "number" },
          "hys": { "type": "number", "minimum": 0 },
          "win_s": { "type": "integer", "minimum": 1 }
        },
        "if": { "properties": { "op": { "enum": ["fall", "rise"] } } },
        "then": {
          "required": ["win_s"],
          "properties": { "thr": { "exclusiveMinimum": 0 } }
        },
        "else": { "not": { "required": ["win_s"] } }
      }
    }
  }
})";

} // namespace rdws::device_config
