#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>

namespace rdws::device_config {

struct DeviceConfig {
  std::string id;
  std::string deviceId;
  std::string config; // JSON string (from JSONB)
  std::string createdAt;
  std::string updatedAt; // empty if NULL
};

struct DeviceConfigUpdate {
  std::string configJson; // serialized JSON
};

// device_config is 1:1 with device — the row is created by a trigger in Postgres alongside the
// device (db/migrations/V3__device_config_one_to_one.sql), never by the application. Therefore,
// the repository does not have create()/remove(): it always exists, only findByDeviceId/update.
class IDeviceConfigRepository {
public:
  virtual ~IDeviceConfigRepository() = default;

  [[nodiscard]] virtual std::optional<DeviceConfig> findByDeviceId(const std::string& deviceId) = 0;
  [[nodiscard]] virtual bool update(const std::string& deviceId,
                                    const DeviceConfigUpdate& data) = 0;
};

class DeviceConfigRepository : public IDeviceConfigRepository {
public:
  explicit DeviceConfigRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::optional<DeviceConfig> findByDeviceId(const std::string& deviceId) override;
  [[nodiscard]] bool update(const std::string& deviceId, const DeviceConfigUpdate& data) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static DeviceConfig configFromRow(rdws::database::IResultSet& rs);
};

} // namespace rdws::device_config
