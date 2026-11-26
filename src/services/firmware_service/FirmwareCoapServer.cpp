// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: AGPL-3.0-only

#include "FirmwareCoapServer.h"

#include "../../shared/utils/logger.h"

#include <arpa/inet.h>
#include <array>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <string_view>

namespace rdws::firmware {

namespace logger = rdws::utils::logger;

namespace {

using Kind = DeliveryEvent::Kind;
using Status = DeliveryReply::Status;

constexpr unsigned kIoWaitMs = 500;
// Shorter than libcoap's 300 s for the same reason as the ingestion: a station that resets reconnects
// from the same port, and its new handshake reaches the stale session
constexpr unsigned kSessionTimeoutSec = 60;
constexpr std::size_t kMaxBlockOptionSize = 3; // RFC 7959
constexpr unsigned kSzxMask = 7;
constexpr unsigned kMoreFlag = 8;
constexpr unsigned kNumShift = 4;

std::string_view view(const coap_bin_const_t* bin) {
  return {reinterpret_cast<const char*>(bin->s), bin->length};
}

// The block asked for; block 0 of 1024 bytes without the option, nullopt when it is malformed.
// libcoap's parser isn't used: it answers the reserved size as if there were no option.
std::optional<BlockNumber> requestedBlock(const coap_pdu_t* request) {
  coap_opt_iterator_t it;
  const coap_opt_t* option = coap_check_option(request, COAP_OPTION_BLOCK2, &it);
  if (option == nullptr) {
    return BlockNumber{};
  }
  if (coap_opt_length(option) > kMaxBlockOptionSize) {
    return std::nullopt;
  }
  const unsigned value = coap_decode_var_bytes(coap_opt_value(option), coap_opt_length(option));
  return BlockNumber{.num = value >> kNumShift, .szx = value & kSzxMask};
}

bool addUint(coap_pdu_t* pdu, coap_option_num_t number, unsigned value) {
  std::array<std::uint8_t, 4> buffer{};
  const auto length = coap_encode_var_safe(buffer.data(), buffer.size(), value);
  return coap_add_option(pdu, number, length, buffer.data()) != 0;
}

// In the order of their numbers, as libcoap wants
bool addContent(coap_pdu_t* response, const DeliveryReply& reply) {
  const unsigned block = reply.block.num << kNumShift | (reply.range.more ? kMoreFlag : 0U) |
                         reply.block.szx;
  return coap_add_option(response, COAP_OPTION_ETAG, reply.etag.size(), reply.etag.data()) != 0 &&
         addUint(response, COAP_OPTION_CONTENT_FORMAT, COAP_MEDIATYPE_APPLICATION_OCTET_STREAM) &&
         addUint(response, COAP_OPTION_BLOCK2, block) &&
         (reply.block.num != 0 ||
          addUint(response, COAP_OPTION_SIZE2, static_cast<unsigned>(reply.imageSize))) &&
         coap_add_data(response, reply.data.size(), reply.data.data()) != 0;
}

void log(const DeliveryEvent& event, const DeliveryReply& reply) {
  const std::string context = "device=" + event.device + " build=" + event.buildId;
  if (event.kind == Kind::Started) {
    logger::info("Firmware download started",
                 context + " from_block=" + std::to_string(reply.block.num));
    return;
  }
  logger::info(event.kind == Kind::Finished ? "Firmware download finished"
                                            : "Firmware download abandoned",
               context + " bytes=" + std::to_string(event.bytes) +
                   " seconds=" + std::to_string(event.elapsed.count() / 1000));
}

} // namespace

void FirmwareCoapServer::onGet(coap_session_t* session, const coap_pdu_t* request,
                               coap_pdu_t* response) {
  // Asked at each request: a revoked credential loses access in an open session too
  const auto* identity = coap_session_get_psk_identity(session);
  const auto device = identity != nullptr ? credentials_.device(view(identity)) : std::nullopt;
  if (!device || device->publicId.empty()) {
    logger::warn("Firmware request without a known device, refused", "");
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_UNAUTHORIZED);
    return;
  }
  const auto block = requestedBlock(request);
  if (!block) {
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_BAD_REQUEST);
    return;
  }

  const auto reply = delivery_.get(device->publicId, *block, ImageDelivery::Clock::now());
  for (const auto& event : reply.events) {
    log(event, reply);
  }
  switch (reply.status) {
  case Status::Content:
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
    if (!addContent(response, reply)) {
      logger::error("Firmware block doesn't fit in the answer", "device=" + device->publicId);
      coap_pdu_set_code(response, COAP_RESPONSE_CODE_INTERNAL_ERROR);
    }
    break;
  case Status::BadBlock:
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_BAD_REQUEST);
    break;
  case Status::NoImage:
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_NOT_FOUND);
    break;
  case Status::BrokenFile:
    logger::error("Firmware file missing, or not of the registered size",
                  "device=" + device->publicId);
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_INTERNAL_ERROR);
    break;
  case Status::Busy:
  case Status::Unavailable:
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE);
    addUint(response, COAP_OPTION_MAXAGE, static_cast<unsigned>(reply.retryAfter.count()));
    break;
  }
}

bool FirmwareCoapServer::serve(const std::atomic<bool>& running) {
  static std::once_flag once;
  std::call_once(once, [] { coap_startup(); });

  coap_context_t* ctx = coap_new_context(nullptr);
  if (ctx == nullptr) {
    return false;
  }
  coap_context_set_app_data(ctx, this);
  // No libcoap block mode: it would keep the image per session and answer the next blocks by itself
  coap_context_set_block_mode(ctx, 0);
  coap_context_set_session_timeout(ctx, kSessionTimeoutSec);

  coap_dtls_spsk_t psk{};
  psk.version = COAP_DTLS_SPSK_SETUP_VERSION;
  psk.id_call_back_arg = &credentials_;
  psk.validate_id_call_back = [](coap_bin_const_t* identity, coap_session_t*, void* arg) {
    return static_cast<rdws::coap::PskCache*>(arg)->keyForHandshake(view(identity));
  };

  coap_address_t address;
  coap_address_init(&address);
  address.addr.sin.sin_family = AF_INET;
  address.addr.sin.sin_port = htons(port_);
  const bool opened = inet_pton(AF_INET, host_.c_str(), &address.addr.sin.sin_addr) == 1 &&
                      coap_context_set_psk2(ctx, &psk) != 0 &&
                      coap_new_endpoint(ctx, &address, COAP_PROTO_DTLS) != nullptr;
  if (opened) {
    coap_str_const_t uri{2, reinterpret_cast<const std::uint8_t*>("fw")};
    coap_resource_t* resource = coap_resource_init(&uri, 0);
    coap_register_request_handler(resource, COAP_REQUEST_GET,
                                  [](coap_resource_t*, coap_session_t* session,
                                     const coap_pdu_t* request, const coap_string_t*,
                                     coap_pdu_t* response) {
                                    static_cast<FirmwareCoapServer*>(
                                        coap_context_get_app_data(coap_session_get_context(session)))
                                        ->onGet(session, request, response);
                                  });
    coap_add_resource(ctx, resource);
    logger::info("Firmware delivery listening",
                 "coaps://" + host_ + ":" + std::to_string(port_) + "/fw");
    while (running.load()) {
      coap_io_process(ctx, kIoWaitMs);
    }
  }
  coap_free_context(ctx);
  return opened;
}

} // namespace rdws::firmware
