// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <coap3/coap.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rdws::coap {

// The device a PSK identity belongs to
struct PskDevice {
  std::string id;       // internal: readings, telemetry and logs carry it
  std::string publicId; // the API's device id; empty only if device_service didn't send it
};

// PSKs of the active devices, for a CoAP/DTLS server: loaded from device_credential.list_active,
// read by libcoap's callback. The refresh runs in another thread, so each load replaces an
// immutable snapshot instead of changing the one in use.
class PskCache {
public:
  // Replaces the content with the capability's answer (`{"data": [...]}`). False, keeping the
  // previous content, if it isn't one. An entry without an identity, a device or a valid key is
  // left out, and counted in skipped().
  bool load(const std::string& envelope);

  // False until the first successful load()
  [[nodiscard]] bool loaded() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t skipped() const;

  // The device of an identity, or nullopt: never issued, revoked or rotated
  [[nodiscard]] std::optional<PskDevice> device(std::string_view identity) const;

  // For validate_id_call_back, always in the CoAP thread: libcoap reads the key after the callback
  // returns, so the snapshot it belongs to lives until the next call.
  [[nodiscard]] const coap_bin_const_t* keyForHandshake(std::string_view identity);

private:
  struct Entry {
    std::string key;
    PskDevice device;
    coap_bin_const_t bin{}; // points to `key`
  };
  struct Snapshot {
    std::unordered_map<std::string, Entry> entries;
    std::size_t skipped = 0;
  };

  [[nodiscard]] std::shared_ptr<const Snapshot> snapshot() const;

  mutable std::mutex mutex_;
  std::shared_ptr<const Snapshot> current_; // null until the first load()
  std::shared_ptr<const Snapshot> inHandshake_;
};

} // namespace rdws::coap
