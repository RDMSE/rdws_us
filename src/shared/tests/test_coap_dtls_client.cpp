// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

// Local, self-contained verification that CoapDtlsClient completes a real DTLS-PSK
// handshake and delivers a payload — spins up a minimal libcoap PSK server in-process
// (no IngestionService exists yet; see Plano_SensorSimulatorService_Implementacao.md).
#include "coap_dtls_client.h"

#include <coap3/coap.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace {

// One port per test: ctest runs each case as its own process, in parallel (gtest_discover_tests).
constexpr uint16_t kPortDelivers = 25683;
constexpr uint16_t kPortWrongPsk = 25684;
constexpr uint16_t kPortUnavailable = 25685;
constexpr uint16_t kPortBadRequest = 25686;
constexpr char kPskIdentity[] = "sim-device-1";
constexpr char kPskKey[] = "unit-test-psk-key-32-bytes-long";

std::atomic<bool> gReceivedExpectedPayload{false};
std::atomic<int> gReceivedContentFormat{-1};
// What the test server answers — 5.03 stands in for IngestionService refusing the pack.
std::atomic<coap_pdu_code_t> gResponseCode{COAP_RESPONSE_CODE_CHANGED};

void handlePost(coap_resource_t* /*resource*/, coap_session_t* /*session*/,
               const coap_pdu_t* request, const coap_string_t* /*query*/,
               coap_pdu_t* response) {
  const uint8_t* data = nullptr;
  size_t len = 0;
  size_t offset = 0;
  size_t total = 0;
  coap_get_data_large(request, &len, &data, &offset, &total);
  if (len == 4 && std::memcmp(data, "ping", 4) == 0) {
    gReceivedExpectedPayload = true;
  }
  coap_opt_iterator_t it;
  if (const coap_opt_t* opt = coap_check_option(request, COAP_OPTION_CONTENT_FORMAT, &it)) {
    gReceivedContentFormat =
        static_cast<int>(coap_decode_var_bytes(coap_opt_value(opt), coap_opt_length(opt)));
  }
  coap_pdu_set_code(response, gResponseCode.load());
}

const coap_bin_const_t* pskLookup(coap_bin_const_t* identity, coap_session_t* /*session*/,
                                  void* /*arg*/) {
  static const coap_bin_const_t key{sizeof(kPskKey) - 1,
                                    reinterpret_cast<const uint8_t*>(kPskKey)};
  if (identity->length == sizeof(kPskIdentity) - 1 &&
      std::memcmp(identity->s, kPskIdentity, identity->length) == 0) {
    return &key;
  }
  return nullptr;
}

class TestCoapServer {
public:
  explicit TestCoapServer(uint16_t port) {
    coap_startup();
    ctx_ = coap_new_context(nullptr);

    coap_dtls_spsk_t setupData{};
    setupData.version = COAP_DTLS_SPSK_SETUP_VERSION;
    setupData.validate_id_call_back = pskLookup;
    setupData.psk_info.hint = {0, nullptr};
    setupData.psk_info.key = {sizeof(kPskKey) - 1, reinterpret_cast<const uint8_t*>(kPskKey)};
    coap_context_set_psk2(ctx_, &setupData);

    coap_address_t addr;
    coap_address_init(&addr);
    addr.addr.sin.sin_family = AF_INET;
    addr.addr.sin.sin_port = htons(port);
    addr.addr.sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    coap_new_endpoint(ctx_, &addr, COAP_PROTO_DTLS);

    coap_str_const_t rootUri{0, reinterpret_cast<const uint8_t*>("")};
    coap_resource_t* resource = coap_resource_init(&rootUri, 0);
    coap_register_request_handler(resource, COAP_REQUEST_POST, handlePost);
    coap_add_resource(ctx_, resource);

    running_ = true;
    thread_ = std::thread([this] {
      while (running_) {
        coap_io_process(ctx_, 100);
      }
    });
  }

  ~TestCoapServer() {
    running_ = false;
    thread_.join();
    coap_free_context(ctx_);
  }

private:
  coap_context_t* ctx_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

} // namespace

TEST(CoapDtlsClientTest, SendConfirmable_CompletesDtlsHandshakeAndDeliversPayload) {
  gReceivedExpectedPayload = false;
  gReceivedContentFormat = -1;
  gResponseCode = COAP_RESPONSE_CODE_CHANGED;
  TestCoapServer server(kPortDelivers);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  rdws::coap::CoapDtlsClient client("127.0.0.1", kPortDelivers, 5000);
  const std::vector<uint8_t> payload = {'p', 'i', 'n', 'g'};

  const bool ok =
      client.sendConfirmable(kPskIdentity, kPskKey, payload, COAP_MEDIATYPE_APPLICATION_SENML_JSON);

  EXPECT_TRUE(ok);
  EXPECT_TRUE(gReceivedExpectedPayload.load());
  EXPECT_EQ(gReceivedContentFormat.load(), COAP_MEDIATYPE_APPLICATION_SENML_JSON);
  EXPECT_EQ(client.lastResponseCode(), "2.04");
}

// A response that isn't 2.xx means the server didn't take the data: the caller must keep
// it. Before, any response — 5.03 included — counted as delivered and the simulator
// cleared its buffered readings.
TEST(CoapDtlsClientTest, SendConfirmable_ServiceUnavailableIsNotDelivered) {
  gResponseCode = COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE;
  TestCoapServer server(kPortUnavailable);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  rdws::coap::CoapDtlsClient client("127.0.0.1", kPortUnavailable, 5000);
  const std::vector<uint8_t> payload = {'p', 'i', 'n', 'g'};

  const bool ok =
      client.sendConfirmable(kPskIdentity, kPskKey, payload, COAP_MEDIATYPE_APPLICATION_SENML_JSON);

  EXPECT_FALSE(ok);
  EXPECT_EQ(client.lastResponseCode(), "5.03");
  gResponseCode = COAP_RESPONSE_CODE_CHANGED;
}

TEST(CoapDtlsClientTest, SendConfirmable_BadRequestIsNotDelivered) {
  gResponseCode = COAP_RESPONSE_CODE_BAD_REQUEST;
  TestCoapServer server(kPortBadRequest);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  rdws::coap::CoapDtlsClient client("127.0.0.1", kPortBadRequest, 5000);
  const std::vector<uint8_t> payload = {'p', 'i', 'n', 'g'};

  EXPECT_FALSE(
      client.sendConfirmable(kPskIdentity, kPskKey, payload, COAP_MEDIATYPE_APPLICATION_SENML_JSON));
  EXPECT_EQ(client.lastResponseCode(), "4.00");
  gResponseCode = COAP_RESPONSE_CODE_CHANGED;
}

TEST(CoapDtlsClientTest, SendConfirmable_WrongPskFailsHandshake) {
  TestCoapServer server(kPortWrongPsk);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  rdws::coap::CoapDtlsClient client("127.0.0.1", kPortWrongPsk, 2000);
  const std::vector<uint8_t> payload = {'p', 'i', 'n', 'g'};

  const bool ok = client.sendConfirmable("unknown-identity", "wrong-key-not-provisioned-32byte",
                                         payload, COAP_MEDIATYPE_APPLICATION_SENML_JSON);

  EXPECT_FALSE(ok);
  EXPECT_EQ(client.lastResponseCode(), "none");
}
