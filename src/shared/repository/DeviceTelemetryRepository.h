// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>

namespace rdws::device_telemetry {

// A device's latest diagnostics snapshot: the latest device_telemetry row carrying boot_count,
// which the firmware and the simulator send in every cycle's diagnostics (crash records and the
// per-pack seq rows don't carry it).
struct DiagnosticsSnapshot {
  std::string timestamp; // ISO 8601 UTC
  std::string dataJson;  // the row's JSONB as a JSON object
};

class IDeviceTelemetryRepository {
public:
  virtual ~IDeviceTelemetryRepository() = default;

  // Idempotent upsert on UNIQUE(device_id, timestamp) (V11): an existing row gets the new
  // keys merged in (data || EXCLUDED.data), so a redelivered queue message is a no-op and a
  // second pack at the same instant adds its metrics. `dataJson` is a JSON object.
  [[nodiscard]] virtual bool upsert(const std::string& deviceId, const std::string& timestamp,
                                    const std::string& dataJson) = 0;

  // Widens the span of a config version on a device (V16, Plano_Indices_Derivados.md D1.1):
  // LEAST/GREATEST make a redelivered or out-of-order pack a no-op.
  [[nodiscard]] virtual bool upsertConfigSpan(const std::string& deviceId, int version,
                                              const std::string& firstTs,
                                              const std::string& lastTs) = 0;

  // Retention (Plano_Telemetria.md, Fase 3 R4): drops rows older than `days`.
  [[nodiscard]] virtual bool deleteOlderThan(int days) = 0;

  // nullopt when the device never sent diagnostics.
  [[nodiscard]] virtual std::optional<DiagnosticsSnapshot>
  latestDiagnostics(const std::string& deviceId) = 0;
};

class DeviceTelemetryRepository : public IDeviceTelemetryRepository {
public:
  explicit DeviceTelemetryRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] bool upsert(const std::string& deviceId, const std::string& timestamp,
                            const std::string& dataJson) override;
  [[nodiscard]] bool upsertConfigSpan(const std::string& deviceId, int version,
                                      const std::string& firstTs,
                                      const std::string& lastTs) override;
  [[nodiscard]] bool deleteOlderThan(int days) override;
  [[nodiscard]] std::optional<DiagnosticsSnapshot>
  latestDiagnostics(const std::string& deviceId) override;

private:
  rdws::database::IDatabase& db_;
};

} // namespace rdws::device_telemetry
