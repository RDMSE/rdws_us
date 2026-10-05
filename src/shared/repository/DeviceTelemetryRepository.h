#pragma once

#include "../database/idatabase.h"

#include <string>

namespace rdws::device_telemetry {

class IDeviceTelemetryRepository {
public:
  virtual ~IDeviceTelemetryRepository() = default;

  // Idempotent upsert on UNIQUE(device_id, timestamp) (V11): an existing row gets the new
  // keys merged in (data || EXCLUDED.data), so a redelivered queue message is a no-op and a
  // second pack at the same instant adds its metrics. `dataJson` is a JSON object.
  [[nodiscard]] virtual bool upsert(const std::string& deviceId, const std::string& timestamp,
                                    const std::string& dataJson) = 0;
};

class DeviceTelemetryRepository : public IDeviceTelemetryRepository {
public:
  explicit DeviceTelemetryRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] bool upsert(const std::string& deviceId, const std::string& timestamp,
                            const std::string& dataJson) override;

private:
  rdws::database::IDatabase& db_;
};

} // namespace rdws::device_telemetry
