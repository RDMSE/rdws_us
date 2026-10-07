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
// channel is a board detail the firmware knows (decided 2026-10-07).
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
          "sensor_id": { "type": "integer", "minimum": 1, "maximum": 4294967295 }
        }
      }
    }
  }
})";

} // namespace rdws::device_config
