#pragma once

// SenML JSON (RFC 8428) pack parser for IngestionService (Plano_Telemetria.md, D1/D2/DP3).
// Pure function, no I/O: resolves base fields into self-contained records. Classifying names
// (sensor / diagnostic / metadata), ownership and unit conversion are the caller's job.

#include <tl/expected.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rdws::senml {

struct Record {
  std::string name; // bn + n
  double time = 0;  // absolute epoch seconds (bt + t, or relative to `now`)
  std::string unit; // u, or bu when absent; empty if neither
  std::optional<double> value;            // v (+ bv)
  std::optional<bool> boolValue;          // vb
  std::optional<std::string> stringValue; // vs
  uint16_t flags = 0;                     // fl_ extension (DP3); 0 when absent
};

// Parses a SenML JSON pack. `now` (epoch seconds) resolves times below 2^28, which RFC 8428
// §4.5.3 defines as relative to the current time. Any malformed record fails the whole pack,
// as does an unknown field ending in '_' (must-understand, RFC 8428 §4.4).
[[nodiscard]] tl::expected<std::vector<Record>, std::string> parse(const std::string& body,
                                                                  double now);

} // namespace rdws::senml
