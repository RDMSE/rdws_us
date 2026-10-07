#include "DeviceActivityRepository.h"

namespace rdws::device {

bool DeviceActivityRepository::touchLastSeen(const std::string& deviceId) {
  const std::string command =
      "UPDATE devices SET last_seen = now() "
      "WHERE id = $1 AND (last_seen IS NULL OR last_seen < now() - interval '1 minute')";
  return db_.execCommand(command, {deviceId});
}

} // namespace rdws::device
