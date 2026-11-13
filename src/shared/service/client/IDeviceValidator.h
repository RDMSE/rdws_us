// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace rdws::device {

// Result of resolving a device's public id. When found, id is the device's internal id (what a
// child row's FK stores). When found is false, statusCode carries the cause so callers can
// tell "device not found" (404) from transient failures (gateway disconnected, invoke
// timeout/503) instead of collapsing both into a 404.
struct DeviceValidation {
  bool found = false;
  std::string id; // internal, set when found
  int statusCode = 404;
  std::string errorMessage;
};

// Narrow interface used by domain services that only need to check a device_id they received
// (a public id) and get the internal id their FK needs, without pulling in the rest of
// IDeviceRepository's CRUD surface.
class IDeviceValidator {
public:
  virtual ~IDeviceValidator() = default;

  [[nodiscard]] virtual DeviceValidation resolve(const std::string& devicePublicId) = 0;
};

} // namespace rdws::device
