#pragma once

#include "../database/idatabase.h"

#include <string>

namespace rdws::device {

// devices.last_seen (V14): when a station last got data accepted by the server.
class IDeviceActivityRepository {
public:
  virtual ~IDeviceActivityRepository() = default;

  // Sets last_seen to now() unless it's already within the last minute — the SQL guard
  // that, with the caller's own throttle, keeps this from being one UPDATE per reading.
  [[nodiscard]] virtual bool touchLastSeen(const std::string& deviceId) = 0;
};

class DeviceActivityRepository : public IDeviceActivityRepository {
public:
  explicit DeviceActivityRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] bool touchLastSeen(const std::string& deviceId) override;

private:
  rdws::database::IDatabase& db_;
};

} // namespace rdws::device
