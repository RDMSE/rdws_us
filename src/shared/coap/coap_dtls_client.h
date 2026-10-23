#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rdws::coap {

// CoAP Content-Format of a SenML JSON pack (application/senml+json, RFC 8428)
inline constexpr uint16_t kContentFormatSenmlJson = 110;

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
  // confirmable POST with `payload` as the body (Content-Format `contentFormat`, e.g. 110 =
  // application/senml+json), and waits for the ACK/response. Returns true iff a 2.xx
  // response arrived before the timeout — a 4.xx/5.xx (e.g. 5.03 when IngestionService
  // couldn't hand the readings to RabbitMQ) means the server did NOT take the data, so the
  // caller must keep it. Before, any response counted as delivered.
  [[nodiscard]] bool sendConfirmable(const std::string& pskIdentity, const std::string& pskKey,
                                     const std::vector<uint8_t>& payload,
                                     uint16_t contentFormat) const;

  // Response code of the last sendConfirmable() as "C.DD" (e.g. "2.04", "5.03"), or "none"
  // when no response arrived — for logging.
  [[nodiscard]] const std::string& lastResponseCode() const { return lastResponseCode_; }

private:
  std::string host_;
  uint16_t port_;
  unsigned int timeoutMs_;
  mutable std::string lastResponseCode_ = "none";
};

} // namespace rdws::coap
