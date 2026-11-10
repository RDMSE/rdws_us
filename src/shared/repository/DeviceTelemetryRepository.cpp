// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

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

bool DeviceTelemetryRepository::upsertConfigSpan(const std::string& deviceId, int version,
                                                 const std::string& firstTs,
                                                 const std::string& lastTs) {
  const std::string command =
      "INSERT INTO device_config_spans (device_id, version, first_ts, last_ts) "
      "VALUES ($1, $2, $3, $4) "
      "ON CONFLICT (device_id, version) DO UPDATE SET "
      "first_ts = LEAST(device_config_spans.first_ts, EXCLUDED.first_ts), "
      "last_ts = GREATEST(device_config_spans.last_ts, EXCLUDED.last_ts)";
  return db_.execCommand(command, {deviceId, std::to_string(version), firstTs, lastTs});
}

bool DeviceTelemetryRepository::deleteOlderThan(int days) {
  return db_.execCommand(
      "DELETE FROM device_telemetry WHERE timestamp < now() - make_interval(days => $1::int)",
      {std::to_string(days)});
}

std::optional<DiagnosticsSnapshot>
DeviceTelemetryRepository::latestDiagnostics(const std::string& deviceId) {
  const std::string query =
      "SELECT to_char(timestamp AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS ts, "
      "data::text AS data FROM device_telemetry "
      "WHERE device_id = $1 AND data ? 'boot_count' ORDER BY timestamp DESC LIMIT 1";
  auto rs = db_.execQuery(query, {deviceId});
  if (!rs->next()) {
    return std::nullopt;
  }
  return DiagnosticsSnapshot{.timestamp = rs->getString("ts"), .dataJson = rs->getString("data")};
}

} // namespace rdws::device_telemetry
