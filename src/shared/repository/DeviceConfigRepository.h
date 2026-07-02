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

struct DeviceConfigCreate {
  std::string deviceId;
  std::string configJson; // serialized JSON
};

struct DeviceConfigUpdate {
  std::string configJson; // serialized JSON
};

class IDeviceConfigRepository {
public:
  virtual ~IDeviceConfigRepository() = default;

  [[nodiscard]] virtual std::optional<DeviceConfig> findByDeviceId(const std::string& deviceId) = 0;
  [[nodiscard]] virtual std::string create(const DeviceConfigCreate& data) = 0;
  [[nodiscard]] virtual bool update(const std::string& deviceId,
                                    const DeviceConfigUpdate& data) = 0;
  [[nodiscard]] virtual bool remove(const std::string& deviceId) = 0;
};

class DeviceConfigRepository : public IDeviceConfigRepository {
public:
  explicit DeviceConfigRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::optional<DeviceConfig> findByDeviceId(const std::string& deviceId) override;
  [[nodiscard]] std::string create(const DeviceConfigCreate& data) override;
  [[nodiscard]] bool update(const std::string& deviceId, const DeviceConfigUpdate& data) override;
  [[nodiscard]] bool remove(const std::string& deviceId) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static DeviceConfig configFromRow(rdws::database::IResultSet& rs);
};

} // namespace rdws::device_config
