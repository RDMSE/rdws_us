// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

// The real endpoint, over DTLS, with a client that sends whatever option it is told to.

#include "services/firmware_service/FirmwareCoapServer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace rdws::firmware;
using rdws::coap::PskCache;

namespace {

using Bytes = std::vector<std::uint8_t>;

const std::string kSha = "0123456789abcdef" + std::string(48, 'a');

Bytes content(std::size_t size) {
  Bytes data(size);
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = static_cast<std::uint8_t>(i * 31 + i / 256);
  }
  return data;
}

class Targets : public IDeliveryTargets {
public:
  std::map<std::string, DeliveredImage> images;
  std::atomic<bool> down{false};
  std::optional<DeliveredImage> find(const std::string& device) override {
    if (down) {
      throw std::runtime_error("down");
    }
    const auto it = images.find(device);
    return it == images.end() ? std::nullopt : std::optional(it->second);
  }
};

class Blocks : public IImageBlocks {
public:
  Bytes file = content(5000);
  std::atomic<bool> missing{false};
  std::optional<Bytes> readAt(const std::string&, std::size_t, std::size_t offset,
                              std::size_t length) override {
    if (missing) {
      return std::nullopt;
    }
    const auto from = file.begin() + static_cast<std::ptrdiff_t>(offset);
    return Bytes(from, from + static_cast<std::ptrdiff_t>(length));
  }
};

struct Answer {
  int code = 0; // 205, 404...; 0 without an answer
  Bytes data;
  Bytes etag;
  std::optional<unsigned> block2;
  std::optional<unsigned> size2;
  std::optional<unsigned> format;
  std::optional<unsigned> maxAge;
  [[nodiscard]] bool more() const { return block2 && (*block2 & 8U) != 0; }
};

// One DTLS session
class Client {
public:
  Client(std::uint16_t port, const std::string& identity, const std::string& key)
      : identity_(identity), key_(key) {
    coap_startup();
    ctx_ = coap_new_context(nullptr);
    coap_context_set_block_mode(ctx_, 0);
    coap_context_set_app_data(ctx_, this);
    coap_register_response_handler(ctx_, onResponse);
    coap_address_t to;
    coap_address_init(&to);
    to.addr.sin.sin_family = AF_INET;
    to.addr.sin.sin_port = htons(port);
    to.addr.sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    coap_dtls_cpsk_t psk{};
    psk.version = COAP_DTLS_CPSK_SETUP_VERSION;
    psk.psk_info.identity = {identity_.size(),
                             reinterpret_cast<const std::uint8_t*>(identity_.data())};
    psk.psk_info.key = {key_.size(), reinterpret_cast<const std::uint8_t*>(key_.data())};
    session_ = coap_new_client_session_psk2(ctx_, nullptr, &to, COAP_PROTO_DTLS, &psk);
  }
  ~Client() {
    coap_session_release(session_);
    coap_free_context(ctx_);
  }

  Answer get(const std::optional<Bytes>& block2, const std::string& path = "fw",
             coap_pdu_code_t method = COAP_REQUEST_CODE_GET) {
    answer_ = {};
    done_ = false;
    coap_pdu_t* pdu = coap_pdu_init(COAP_MESSAGE_CON, method, coap_new_message_id(session_),
                                    coap_session_max_pdu_size(session_));
    Bytes token(8); // the longest
    std::size_t length = 0;
    coap_session_new_token(session_, &length, token.data());
    coap_add_token(pdu, token.size(), token.data());
    coap_add_option(pdu, COAP_OPTION_URI_PATH, path.size(),
                    reinterpret_cast<const std::uint8_t*>(path.data()));
    if (block2) {
      coap_add_option(pdu, COAP_OPTION_BLOCK2, block2->size(), block2->data());
    }
    coap_send(session_, pdu);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done_ && std::chrono::steady_clock::now() < end) {
      coap_io_process(ctx_, 50);
    }
    return answer_;
  }
  Answer block(unsigned num, unsigned szx = 6) {
    const unsigned value = num << 4 | szx;
    Bytes option(4);
    option.resize(coap_encode_var_safe(option.data(), option.size(), value));
    return get(option);
  }

private:
  static coap_response_t onResponse(coap_session_t* session, const coap_pdu_t*,
                                    const coap_pdu_t* pdu, const coap_mid_t) {
    auto* self = static_cast<Client*>(coap_context_get_app_data(coap_session_get_context(session)));
    const auto code = coap_pdu_get_code(pdu);
    auto& a = self->answer_;
    a.code = COAP_RESPONSE_CLASS(code) * 100 + (code & 0x1f);
    std::size_t length = 0;
    const std::uint8_t* data = nullptr;
    if (coap_get_data(pdu, &length, &data) != 0) {
      a.data.assign(data, data + length);
    }
    const auto number = [pdu](coap_option_num_t option) -> std::optional<unsigned> {
      coap_opt_iterator_t it;
      const auto* o = coap_check_option(pdu, option, &it);
      return o ? std::optional(coap_decode_var_bytes(coap_opt_value(o), coap_opt_length(o)))
               : std::nullopt;
    };
    coap_opt_iterator_t it;
    if (const auto* etag = coap_check_option(pdu, COAP_OPTION_ETAG, &it)) {
      a.etag.assign(coap_opt_value(etag), coap_opt_value(etag) + coap_opt_length(etag));
    }
    a.block2 = number(COAP_OPTION_BLOCK2);
    a.size2 = number(COAP_OPTION_SIZE2);
    a.format = number(COAP_OPTION_CONTENT_FORMAT);
    a.maxAge = number(COAP_OPTION_MAXAGE);
    self->done_ = true;
    return COAP_RESPONSE_OK;
  }

  std::string identity_;
  std::string key_;
  coap_context_t* ctx_ = nullptr;
  coap_session_t* session_ = nullptr;
  Answer answer_;
  bool done_ = false;
};

// ctest runs each test in a process of its own, in parallel
std::uint16_t freePort() {
  return static_cast<std::uint16_t>(26000 + ::getpid() % 3000);
}

std::string credentials(const std::vector<std::string>& devices) {
  std::string list;
  for (const auto& device : devices) {
    list += std::string(list.empty() ? "" : ",") + R"({"psk_identity":"id-)" + device +
            R"(","psk_key":"6b6579","device_id":"1","device_public_id":")" + device + R"("})";
  }
  return R"({"data":[)" + list + "]}";
}

class FirmwareCoapServerTest : public ::testing::Test {
protected:
  Targets targets;
  Blocks blocks;
  ImageDelivery delivery{targets, blocks, 2};
  PskCache cache;
  std::uint16_t port = freePort();
  FirmwareCoapServer server{cache, delivery, "127.0.0.1", port};
  std::atomic<bool> running{true};
  std::thread thread;

  void SetUp() override {
    for (const auto* device : {"d1", "d2", "d3"}) {
      targets.images[device] = {.buildId = "build-a", .sha256 = kSha, .size = 5000};
    }
    ASSERT_TRUE(cache.load(credentials({"d1", "d2", "d3", "none"})));
    thread = std::thread([this] { EXPECT_TRUE(server.serve(running)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  void TearDown() override {
    running = false;
    thread.join();
  }
  [[nodiscard]] Client client(const std::string& device) const {
    return {port, "id-" + device, "key"};
  }
};

} // namespace

TEST_F(FirmwareCoapServerTest, WholeImage) {
  for (const unsigned szx : {6U, 5U, 2U}) {
    auto c = client("d1");
    Bytes got;
    for (unsigned num = 0;; ++num) {
      const auto a = c.block(num, szx);
      ASSERT_EQ(a.code, 205) << num;
      EXPECT_EQ(a.block2, num << 4 | (a.more() ? 8U : 0U) | szx);
      EXPECT_EQ(a.size2.has_value(), num == 0);
      got.insert(got.end(), a.data.begin(), a.data.end());
      if (!a.more()) {
        break;
      }
    }
    EXPECT_EQ(got, blocks.file) << szx;
  }
}

TEST_F(FirmwareCoapServerTest, FirstBlock) {
  const auto a = client("d1").block(0);
  EXPECT_EQ(a.code, 205);
  EXPECT_EQ(a.data.size(), 1024U); // with the longest token and every option
  EXPECT_EQ(a.etag, (Bytes{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}));
  EXPECT_EQ(a.format, 42U);
  EXPECT_EQ(a.size2, 5000U);
  EXPECT_TRUE(a.more());
}

TEST_F(FirmwareCoapServerTest, WithoutTheOption) {
  const auto a = client("d1").get(std::nullopt);
  EXPECT_EQ(a.code, 205);
  EXPECT_EQ(a.block2, 8U | 6U);
  EXPECT_EQ(a.data.size(), 1024U);
}

TEST_F(FirmwareCoapServerTest, ResumeInAnotherSession) {
  (void)client("d1").block(0);
  const auto a = client("d1").block(4);
  EXPECT_EQ(a.code, 205);
  EXPECT_EQ(a.data, Bytes(blocks.file.begin() + 4096, blocks.file.end()));
  EXPECT_FALSE(a.more());
}

TEST_F(FirmwareCoapServerTest, BadBlocks) {
  auto c = client("d1");
  EXPECT_EQ(c.block(5).code, 400);                          // past the end
  EXPECT_EQ(c.block(0, 7).code, 400);                       // reserved size
  EXPECT_EQ(c.get(Bytes{0xff, 0xff, 0xf6}).code, 400);      // the largest number
  EXPECT_EQ(c.block(0).code, 205);                          // still alive
}

TEST_F(FirmwareCoapServerTest, OnlyThisResourceAndMethod) {
  auto c = client("d1");
  EXPECT_EQ(c.get(std::nullopt, "other").code, 404);
  EXPECT_EQ(c.get(std::nullopt, "fw/../../etc/passwd").code, 404);
  EXPECT_EQ(c.get(std::nullopt, "fw", COAP_REQUEST_CODE_DELETE).code, 405);
}

TEST_F(FirmwareCoapServerTest, NoImage) {
  const auto a = client("none").block(0);
  EXPECT_EQ(a.code, 404);
  EXPECT_TRUE(a.etag.empty());
}

TEST_F(FirmwareCoapServerTest, Busy) {
  EXPECT_EQ(client("d1").block(0).code, 205);
  EXPECT_EQ(client("d2").block(0).code, 205);
  const auto a = client("d3").block(0);
  EXPECT_EQ(a.code, 503);
  EXPECT_EQ(a.maxAge, 3600U);
}

TEST_F(FirmwareCoapServerTest, DatabaseDown) {
  targets.down = true;
  const auto a = client("d1").block(0);
  EXPECT_EQ(a.code, 503);
  EXPECT_EQ(a.maxAge, 30U);
}

TEST_F(FirmwareCoapServerTest, BrokenFile) {
  blocks.missing = true;
  const auto a = client("d1").block(0);
  EXPECT_EQ(a.code, 500);
  EXPECT_TRUE(a.data.empty());
}

TEST_F(FirmwareCoapServerTest, RevokedInAnOpenSession) {
  auto c = client("d1");
  EXPECT_EQ(c.block(0).code, 205);
  ASSERT_TRUE(cache.load(credentials({"d2"})));
  EXPECT_EQ(c.block(1).code, 401);
  EXPECT_EQ(client("d1").block(1).code, 0); // and no new handshake
  EXPECT_EQ(client("d2").block(0).code, 205);
}

TEST_F(FirmwareCoapServerTest, WrongKey) {
  EXPECT_EQ(Client(port, "id-d1", "another").block(0).code, 0);
}

TEST_F(FirmwareCoapServerTest, BadAddress) {
  FirmwareCoapServer second(cache, delivery, "not an address", port);
  EXPECT_FALSE(second.serve(running));
}
