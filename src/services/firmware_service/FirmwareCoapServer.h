// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "../../shared/coap/psk_cache.h"
#include "../../shared/firmware/image_delivery.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace rdws::firmware {

// GET /fw over CoAP/DTLS: the image of the device's target, in blocks (Block2, RFC 7959). The device
// is the session's PSK identity; it doesn't choose the image. Only translates: the rules are in
// ImageDelivery.
class FirmwareCoapServer {
public:
  FirmwareCoapServer(rdws::coap::PskCache& credentials, ImageDelivery& delivery, std::string host,
                     std::uint16_t port)
      : credentials_(credentials), delivery_(delivery), host_(std::move(host)), port_(port) {}

  // Blocks until `running` is false. False when the port can't be opened.
  bool serve(const std::atomic<bool>& running);

private:
  void onGet(coap_session_t* session, const coap_pdu_t* request, coap_pdu_t* response);

  rdws::coap::PskCache& credentials_;
  ImageDelivery& delivery_;
  std::string host_;
  std::uint16_t port_;
};

} // namespace rdws::firmware
