#include "DeviceTelemetryRepository.h"

namespace rdws::device_telemetry {

bool DeviceTelemetryRepository::upsert(const std::string& deviceId, const std::string& timestamp,
                                       const std::string& dataJson) {
  const std::string command =
      "INSERT INTO device_telemetry (device_id, timestamp, data) VALUES ($1, $2, $3::jsonb) "
      "ON CONFLICT (device_id, timestamp) "
      "DO UPDATE SET data = device_telemetry.data || EXCLUDED.data";
  return db_.execCommand(command, {deviceId, timestamp, dataJson});
}

bool DeviceTelemetryRepository::deleteOlderThan(int days) {
  return db_.execCommand(
      "DELETE FROM device_telemetry WHERE timestamp < now() - make_interval(days => $1::int)",
      {std::to_string(days)});
}

} // namespace rdws::device_telemetry
