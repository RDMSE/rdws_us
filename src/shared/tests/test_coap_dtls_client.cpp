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

constexpr uint16_t kTestPort = 25683;
constexpr char kPskIdentity[] = "sim-device-1";
constexpr char kPskKey[] = "unit-test-psk-key-32-bytes-long";

std::atomic<bool> gReceivedExpectedPayload{false};

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
  coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
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
  TestCoapServer() {
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
    addr.addr.sin.sin_port = htons(kTestPort);
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
  TestCoapServer server;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  rdws::coap::CoapDtlsClient client("127.0.0.1", kTestPort, 5000);
  const std::vector<uint8_t> payload = {'p', 'i', 'n', 'g'};

  const bool ok = client.sendConfirmable(kPskIdentity, kPskKey, payload);

  EXPECT_TRUE(ok);
  EXPECT_TRUE(gReceivedExpectedPayload.load());
}

TEST(CoapDtlsClientTest, SendConfirmable_WrongPskFailsHandshake) {
  TestCoapServer server;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  rdws::coap::CoapDtlsClient client("127.0.0.1", kTestPort, 2000);
  const std::vector<uint8_t> payload = {'p', 'i', 'n', 'g'};

  const bool ok = client.sendConfirmable("unknown-identity", "wrong-key-not-provisioned-32byte",
                                         payload);

  EXPECT_FALSE(ok);
}
