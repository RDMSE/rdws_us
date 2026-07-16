#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rdws::coap {

// Thin synchronous wrapper around libcoap for CoAP/DTLS (PSK) client sends —
// SensorSimulatorService opens a new connection per device per transmission cycle
// (Plano_SensorSimulatorService.md), so this client is one-shot: construct, send,
// destroy. Not reused across cycles and not thread-safe for concurrent sendConfirmable
// calls on the same instance.
class CoapDtlsClient {
public:
  // timeoutMs bounds the whole send+wait-for-ack cycle (confirmable CONs get libcoap's
  // own retransmission behavior within that budget).
  CoapDtlsClient(std::string host, uint16_t port, unsigned int timeoutMs = 10000)
      : host_(std::move(host)), port_(port), timeoutMs_(timeoutMs) {}

  // Opens a DTLS session authenticated with the given PSK identity/key, sends a
  // confirmable POST with `payload` as the body, and waits for the ACK/response.
  // Returns true iff a response was received before the connection closes/timeout.
  [[nodiscard]] bool sendConfirmable(const std::string& pskIdentity, const std::string& pskKey,
                                     const std::vector<uint8_t>& payload) const;

private:
  std::string host_;
  uint16_t port_;
  unsigned int timeoutMs_;
};

} // namespace rdws::coap
