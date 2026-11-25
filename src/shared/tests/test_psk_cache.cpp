// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "coap_dtls_client.h"
#include "psk_cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

using rdws::coap::PskCache;

namespace {

std::string entry(const std::string& identity, const std::string& keyHex, const std::string& id,
                  const std::string& publicId = "public-1") {
  return R"({"psk_identity":")" + identity + R"(","psk_key":")" + keyHex + R"(","device_id":")" +
         id + R"(","device_public_id":")" + publicId + R"("})";
}

std::string envelope(const std::string& entries) {
  return R"({"success":true,"data":[)" + entries + "]}";
}

std::string keyOf(PskCache& cache, std::string_view identity) {
  const auto* bin = cache.keyForHandshake(identity);
  return bin ? std::string(reinterpret_cast<const char*>(bin->s), bin->length) : "none";
}

} // namespace

TEST(PskCache, EmptyUntilTheFirstLoad) {
  PskCache cache;
  EXPECT_FALSE(cache.loaded());
  EXPECT_EQ(cache.size(), 0U);
  EXPECT_EQ(cache.keyForHandshake("a"), nullptr);
  EXPECT_FALSE(cache.device("a").has_value());
}

TEST(PskCache, LoadsKeysAndDevices) {
  PskCache cache;
  ASSERT_TRUE(cache.load(envelope(entry("a", "6b657941", "12", "pub-12") + "," +
                                  entry("b", "6B657942", "13"))));
  EXPECT_TRUE(cache.loaded());
  EXPECT_EQ(cache.size(), 2U);
  EXPECT_EQ(keyOf(cache, "a"), "keyA");
  EXPECT_EQ(keyOf(cache, "b"), "keyB");
  ASSERT_TRUE(cache.device("a").has_value());
  EXPECT_EQ(cache.device("a")->id, "12");
  EXPECT_EQ(cache.device("a")->publicId, "pub-12");
  EXPECT_EQ(keyOf(cache, "c"), "none");
  EXPECT_FALSE(cache.device("c").has_value());
}

TEST(PskCache, KeyWithZeroBytes) {
  PskCache cache;
  ASSERT_TRUE(cache.load(envelope(entry("a", "00ff00", "12"))));
  EXPECT_EQ(keyOf(cache, "a"), std::string("\x00\xff\x00", 3));
}

TEST(PskCache, DeviceWithoutPublicId) {
  PskCache cache;
  ASSERT_TRUE(cache.load(R"({"data":[{"psk_identity":"a","psk_key":"6b","device_id":"12"}]})"));
  ASSERT_TRUE(cache.device("a").has_value());
  EXPECT_EQ(cache.device("a")->publicId, "");
}

TEST(PskCache, AnEmptyListIsALoad) {
  PskCache cache;
  ASSERT_TRUE(cache.load(envelope(entry("a", "6b", "12"))));
  ASSERT_TRUE(cache.load(envelope("")));
  EXPECT_TRUE(cache.loaded());
  EXPECT_EQ(cache.size(), 0U);
  EXPECT_FALSE(cache.device("a").has_value());
}

TEST(PskCache, RevokedAndRotated) {
  PskCache cache;
  ASSERT_TRUE(cache.load(envelope(entry("a", "6b", "12") + "," + entry("b", "6b", "13"))));
  EXPECT_EQ(keyOf(cache, "a"), "k");
  EXPECT_EQ(keyOf(cache, "b"), "k");
  ASSERT_TRUE(cache.load(envelope(entry("b", "7a", "13"))));
  EXPECT_FALSE(cache.device("a").has_value());
  EXPECT_EQ(keyOf(cache, "a"), "none");
  EXPECT_EQ(keyOf(cache, "b"), "z");
}

TEST(PskCache, AnAnswerThatIsntOneKeepsThePreviousContent) {
  PskCache cache;
  for (const auto* bad : {"", "not json", "[]", "{}", R"({"data":{}})", R"({"data":null})"}) {
    EXPECT_FALSE(cache.load(bad)) << bad;
    EXPECT_FALSE(cache.loaded()) << bad;
  }
  ASSERT_TRUE(cache.load(envelope(entry("a", "6b", "12"))));
  EXPECT_FALSE(cache.load("not json"));
  EXPECT_FALSE(cache.load("{}"));
  EXPECT_EQ(keyOf(cache, "a"), "k");
}

// One bad row must neither hide the others nor stop the process
TEST(PskCache, InvalidEntriesAreLeftOut) {
  PskCache cache;
  ASSERT_TRUE(cache.load(envelope(
      entry("odd", "6", "1") + "," + entry("nothex", "zz", "2") + "," + entry("nokey", "", "3") + "," +
      entry("nodevice", "6b", "") +
      R"(,{"psk_key":"6b","device_id":"4"},{"psk_identity":"x","psk_key":"6b"})" +
      R"(,{"psk_identity":"y","device_id":"5"},null,7,"text",)" + entry("good", "6b", "12"))));
  EXPECT_EQ(cache.size(), 1U);
  EXPECT_EQ(cache.skipped(), 10U);
  EXPECT_EQ(keyOf(cache, "good"), "k");
  ASSERT_TRUE(cache.load(envelope(entry("good", "6b", "12"))));
  EXPECT_EQ(cache.skipped(), 0U);
}

// libcoap reads the key after the callback has returned
TEST(PskCache, TheKeyOutlivesARefresh) {
  PskCache cache;
  ASSERT_TRUE(cache.load(envelope(entry("a", "6b657941", "12"))));
  const auto* bin = cache.keyForHandshake("a");
  ASSERT_NE(bin, nullptr);
  ASSERT_TRUE(cache.load(envelope("")));
  ASSERT_TRUE(cache.load(envelope(entry("b", "00", "13"))));
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(bin->s), bin->length), "keyA");
}

// Meant for the sanitizers
TEST(PskCache, RefreshDuringHandshakes) {
  PskCache cache;
  const auto full = envelope(entry("a", std::string(64, '6'), "12"));
  ASSERT_TRUE(cache.load(full));
  std::atomic<bool> stop{false};
  std::thread refresh([&] {
    while (!stop) {
      (void)cache.load(full);
    }
  });
  for (int i = 0; i < 20'000; ++i) {
    const auto* bin = cache.keyForHandshake("a");
    ASSERT_NE(bin, nullptr);
    ASSERT_EQ(std::string(reinterpret_cast<const char*>(bin->s), bin->length),
              std::string(32, 'f'));
    ASSERT_EQ(cache.device("a")->id, "12");
  }
  stop = true;
  refresh.join();
}

// Real handshakes, while the cache is replaced all the time
TEST(PskCache, HandshakesWithLibcoap) {
  constexpr uint16_t kPort = 25687; // the others are in test_coap_dtls_client.cpp
  PskCache cache;
  const auto full = envelope(entry("sim", "6b657941", "12"));
  ASSERT_TRUE(cache.load(full));

  coap_startup();
  coap_context_t* ctx = coap_new_context(nullptr);
  coap_dtls_spsk_t setup{};
  setup.version = COAP_DTLS_SPSK_SETUP_VERSION;
  setup.id_call_back_arg = &cache;
  setup.validate_id_call_back = [](coap_bin_const_t* identity, coap_session_t*, void* arg) {
    return static_cast<PskCache*>(arg)->keyForHandshake(
        {reinterpret_cast<const char*>(identity->s), identity->length});
  };
  ASSERT_EQ(coap_context_set_psk2(ctx, &setup), 1);
  coap_address_t addr;
  coap_address_init(&addr);
  addr.addr.sin.sin_family = AF_INET;
  addr.addr.sin.sin_port = htons(kPort);
  addr.addr.sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_NE(coap_new_endpoint(ctx, &addr, COAP_PROTO_DTLS), nullptr);
  coap_str_const_t root{0, reinterpret_cast<const uint8_t*>("")};
  coap_resource_t* resource = coap_resource_init(&root, 0);
  coap_register_request_handler(
      resource, COAP_REQUEST_POST,
      [](coap_resource_t*, coap_session_t*, const coap_pdu_t*, const coap_string_t*,
         coap_pdu_t* response) { coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED); });
  coap_add_resource(ctx, resource);

  std::atomic<bool> stop{false};
  std::thread server([&] {
    while (!stop) {
      coap_io_process(ctx, 50);
    }
  });
  std::thread refresh([&] {
    while (!stop) {
      (void)cache.load(full);
    }
  });
  for (int i = 0; i < 10; ++i) {
    rdws::coap::CoapDtlsClient client("127.0.0.1", kPort, 5000);
    EXPECT_TRUE(client.sendConfirmable("sim", "keyA", {'x'}, 110)) << client.lastResponseCode();
  }
  rdws::coap::CoapDtlsClient wrong("127.0.0.1", kPort, 1500);
  EXPECT_FALSE(wrong.sendConfirmable("sim", "keyB", {'x'}, 110));
  stop = true;
  server.join();
  refresh.join();
  coap_free_context(ctx);
}
