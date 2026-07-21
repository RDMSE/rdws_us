#include "coap_dtls_client.h"

#include <coap3/coap.h>

#include <netdb.h>

#include <chrono>
#include <mutex>

namespace rdws::coap {

namespace {

void ensureLibcoapStarted() {
  static std::once_flag once;
  std::call_once(once, [] { coap_startup(); });
}

struct ResponseState {
  bool received = false;
};

coap_response_t onResponse(coap_session_t* session, const coap_pdu_t* /*sent*/,
                          const coap_pdu_t* /*received*/, const coap_mid_t /*mid*/) {
  auto* state = static_cast<ResponseState*>(
      coap_context_get_app_data(coap_session_get_context(session)));
  if (state) {
    state->received = true;
  }
  return COAP_RESPONSE_OK;
}

} // namespace

bool CoapDtlsClient::sendConfirmable(const std::string& pskIdentity, const std::string& pskKey,
                                     const std::vector<uint8_t>& payload) const {
  ensureLibcoapStarted();

  coap_context_t* ctx = coap_new_context(nullptr);
  if (!ctx) {
    return false;
  }
  // Let libcoap fragment payloads that don't fit in a single PDU (Block1, RFC 7959)
  // instead of coap_add_data() silently dropping whatever doesn't fit — readings
  // accumulate between transmissions, so the payload size isn't bounded.
  coap_context_set_block_mode(ctx, COAP_BLOCK_USE_LIBCOAP);

  ResponseState state;
  coap_context_set_app_data(ctx, &state);
  coap_register_response_handler(ctx, onResponse);

  const coap_str_const_t addrStr = {host_.size(), reinterpret_cast<const uint8_t*>(host_.data())};
  coap_addr_info_t* info =
      coap_resolve_address_info(&addrStr, port_, port_, 0, 0, AI_ADDRCONFIG,
                               COAP_URI_SCHEME_COAPS_BIT, COAP_RESOLVE_TYPE_REMOTE);
  if (!info) {
    coap_free_context(ctx);
    return false;
  }

  coap_dtls_cpsk_t setupData{};
  setupData.version = COAP_DTLS_CPSK_SETUP_VERSION;
  setupData.psk_info.identity.s = reinterpret_cast<const uint8_t*>(pskIdentity.data());
  setupData.psk_info.identity.length = pskIdentity.size();
  setupData.psk_info.key.s = reinterpret_cast<const uint8_t*>(pskKey.data());
  setupData.psk_info.key.length = pskKey.size();

  coap_session_t* session = coap_new_client_session_psk2(ctx, nullptr, &info->addr,
                                                         COAP_PROTO_DTLS, &setupData);
  coap_free_address_info(info);

  if (!session) {
    coap_free_context(ctx);
    return false;
  }

  coap_pdu_t* pdu = coap_pdu_init(COAP_MESSAGE_CON, COAP_REQUEST_CODE_POST,
                                 coap_new_message_id(session),
                                 coap_session_max_pdu_size(session));
  if (!pdu) {
    coap_session_release(session);
    coap_free_context(ctx);
    return false;
  }

  if (!payload.empty()) {
    coap_add_data_large_request(session, pdu, payload.size(), payload.data(), nullptr, nullptr);
  }

  if (coap_send(session, pdu) == COAP_INVALID_MID) {
    coap_session_release(session);
    coap_free_context(ctx);
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
  while (!state.received && std::chrono::steady_clock::now() < deadline) {
    coap_io_process(ctx, 100);
  }

  coap_session_release(session);
  coap_free_context(ctx);
  return state.received;
}

} // namespace rdws::coap
