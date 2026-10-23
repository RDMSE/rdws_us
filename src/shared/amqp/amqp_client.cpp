#include "amqp_client.h"

#include <rabbitmq-c/amqp.h>
#include <rabbitmq-c/tcp_socket.h>
#include <rabbitmq-c/framing.h>

namespace rdws::amqp {

namespace {

constexpr amqp_channel_t kChannel = 1;
constexpr int kFrameMax = 131072;
// How long publish() waits for the broker's confirm before giving up on the connection.
constexpr int kConfirmTimeoutSec = 5;
// Bounds on connect(): the producer runs on IngestionService's single CoAP thread, so an
// unreachable broker host must not block it for the kernel's TCP timeout (minutes).
constexpr int kConnectTimeoutSec = 2;
constexpr int kRpcTimeoutSec = 5;

amqp_connection_state_t asConn(void* conn) {
  return static_cast<amqp_connection_state_t>(conn);
}

bool rpcOk(amqp_connection_state_t conn) {
  const amqp_rpc_reply_t reply = amqp_get_rpc_reply(conn);
  return reply.reply_type == AMQP_RESPONSE_NORMAL;
}

// TCP connect plus the AMQP handshake and later synchronous RPCs, all with timeouts.
bool openSocket(amqp_connection_state_t conn, const std::string& host, uint16_t port) {
  amqp_socket_t* socket = amqp_tcp_socket_new(conn);
  if (socket == nullptr) {
    return false;
  }
  struct timeval connectTimeout {kConnectTimeoutSec, 0};
  struct timeval rpcTimeout {kRpcTimeoutSec, 0};
  return amqp_socket_open_noblock(socket, host.c_str(), port, &connectTimeout) == AMQP_STATUS_OK &&
         amqp_set_handshake_timeout(conn, &connectTimeout) == AMQP_STATUS_OK &&
         amqp_set_rpc_timeout(conn, &rpcTimeout) == AMQP_STATUS_OK;
}

bool declareQueue(amqp_connection_state_t conn, const std::string& queueName) {
  amqp_queue_declare(conn, kChannel, amqp_cstring_bytes(queueName.c_str()),
                    /*passive=*/0, /*durable=*/1, /*exclusive=*/0, /*auto_delete=*/0,
                    amqp_empty_table);
  return rpcOk(conn);
}

} // namespace

// ─── AmqpProducer ──────────────────────────────────────────────────────────

AmqpProducer::AmqpProducer(std::string host, uint16_t port, std::string user,
                          std::string password, std::string queueName)
    : host_(std::move(host)), port_(port), user_(std::move(user)),
      password_(std::move(password)), queueName_(std::move(queueName)) {}

AmqpProducer::~AmqpProducer() { disconnect(); }

bool AmqpProducer::connect() {
  amqp_connection_state_t conn = amqp_new_connection();
  if (!openSocket(conn, host_, port_)) {
    amqp_destroy_connection(conn);
    return false;
  }

  const amqp_rpc_reply_t loginReply =
      amqp_login(conn, "/", 0, kFrameMax, 0, AMQP_SASL_METHOD_PLAIN, user_.c_str(),
                password_.c_str());
  if (loginReply.reply_type != AMQP_RESPONSE_NORMAL) {
    amqp_destroy_connection(conn);
    return false;
  }

  amqp_channel_open(conn, kChannel);
  bool ok = rpcOk(conn) && declareQueue(conn, queueName_);
  if (ok) {
    amqp_confirm_select(conn, kChannel);
    ok = rpcOk(conn);
  }
  if (!ok) {
    amqp_channel_close(conn, kChannel, AMQP_REPLY_SUCCESS);
    amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(conn);
    return false;
  }

  conn_ = conn;
  return true;
}

bool AmqpProducer::publish(const std::string& body) {
  if (conn_ != nullptr && publishConfirmed(body)) {
    return true;
  }
  // Not connected, or the publish failed (and dropped the connection): one fresh attempt.
  if (!connect()) {
    lastError_ = "connect to RabbitMQ failed";
    return false;
  }
  return publishConfirmed(body);
}

bool AmqpProducer::publishConfirmed(const std::string& body) {
  amqp_connection_state_t conn = asConn(conn_);
  amqp_basic_properties_t props;
  props._flags = AMQP_BASIC_DELIVERY_MODE_FLAG;
  props.delivery_mode = 2; // persistent

  const int status = amqp_basic_publish(conn, kChannel, amqp_empty_bytes,
                                        amqp_cstring_bytes(queueName_.c_str()),
                                        /*mandatory=*/0, /*immediate=*/0, &props,
                                        amqp_cstring_bytes(body.c_str()));
  if (status != AMQP_STATUS_OK) {
    dropConnection(std::string("publish failed: ") + amqp_error_string2(status));
    return false;
  }

  // One message in flight at a time, so the next method frame on the channel is its confirm.
  struct timeval timeout {kConfirmTimeoutSec, 0};
  amqp_frame_t frame;
  const int waited = amqp_simple_wait_frame_noblock(conn, &frame, &timeout);
  if (waited != AMQP_STATUS_OK) {
    dropConnection(std::string("waiting for confirm: ") + amqp_error_string2(waited));
    return false;
  }
  if (frame.frame_type == AMQP_FRAME_METHOD && frame.payload.method.id == AMQP_BASIC_ACK_METHOD) {
    return true;
  }
  // basic.nack, or connection.close/channel.close from a broker going down.
  dropConnection(frame.frame_type == AMQP_FRAME_METHOD &&
                         frame.payload.method.id == AMQP_BASIC_NACK_METHOD
                     ? "broker nacked the message"
                     : "unexpected frame from broker");
  return false;
}

void AmqpProducer::dropConnection(std::string reason) {
  lastError_ = std::move(reason);
  amqp_destroy_connection(asConn(conn_));
  conn_ = nullptr;
}

void AmqpProducer::disconnect() {
  if (conn_ == nullptr) {
    return;
  }
  amqp_connection_state_t conn = asConn(conn_);
  amqp_channel_close(conn, kChannel, AMQP_REPLY_SUCCESS);
  amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
  amqp_destroy_connection(conn);
  conn_ = nullptr;
}

// ─── AmqpConsumer ──────────────────────────────────────────────────────────

AmqpConsumer::AmqpConsumer(std::string host, uint16_t port, std::string user,
                          std::string password, std::string queueName)
    : host_(std::move(host)), port_(port), user_(std::move(user)),
      password_(std::move(password)), queueName_(std::move(queueName)) {}

AmqpConsumer::~AmqpConsumer() { disconnect(); }

bool AmqpConsumer::connect() {
  amqp_connection_state_t conn = amqp_new_connection();
  if (!openSocket(conn, host_, port_)) {
    amqp_destroy_connection(conn);
    return false;
  }

  const amqp_rpc_reply_t loginReply =
      amqp_login(conn, "/", 0, kFrameMax, 0, AMQP_SASL_METHOD_PLAIN, user_.c_str(),
                password_.c_str());
  if (loginReply.reply_type != AMQP_RESPONSE_NORMAL) {
    amqp_destroy_connection(conn);
    return false;
  }

  amqp_channel_open(conn, kChannel);
  if (!rpcOk(conn) || !declareQueue(conn, queueName_)) {
    amqp_destroy_connection(conn);
    return false;
  }

  amqp_basic_consume(conn, kChannel, amqp_cstring_bytes(queueName_.c_str()),
                    amqp_empty_bytes, /*no_local=*/0, /*no_ack=*/0, /*exclusive=*/0,
                    amqp_empty_table);
  if (!rpcOk(conn)) {
    amqp_destroy_connection(conn);
    return false;
  }

  conn_ = conn;
  consuming_ = true;
  return true;
}

bool AmqpConsumer::consumeOne(const MessageHandler& handler, unsigned int timeoutMs) {
  if (conn_ == nullptr || !consuming_) {
    return false;
  }
  amqp_connection_state_t conn = asConn(conn_);

  amqp_maybe_release_buffers(conn);

  struct timeval timeout;
  timeout.tv_sec = static_cast<time_t>(timeoutMs / 1000);
  timeout.tv_usec = static_cast<suseconds_t>((timeoutMs % 1000) * 1000);

  amqp_envelope_t envelope;
  const amqp_rpc_reply_t reply = amqp_consume_message(conn, &envelope, &timeout, 0);
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    if (reply.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION &&
        reply.library_error == AMQP_STATUS_TIMEOUT) {
      return false;
    }
    // A socket error, or a frame other than a delivery (connection.close/channel.close from a
    // broker shutting down, basic.cancel for a deleted queue): this connection won't deliver
    // again. Before this, the caller kept calling into the dead socket, which fails at once —
    // a busy loop at 100 % CPU that never reconnected.
    dropConnection(reply.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION
                       ? amqp_error_string2(reply.library_error)
                       : "unexpected frame from broker");
    return false;
  }

  const std::string body(static_cast<const char*>(envelope.message.body.bytes),
                         envelope.message.body.len);
  const int status =
      handler(body) == Outcome::Ack
          ? amqp_basic_ack(conn, kChannel, envelope.delivery_tag, /*multiple=*/0)
          : amqp_basic_nack(conn, kChannel, envelope.delivery_tag, /*multiple=*/0, /*requeue=*/1);
  amqp_destroy_envelope(&envelope);
  if (status != AMQP_STATUS_OK) {
    // The broker returns the unacked delivery to the queue when the connection closes, and the
    // writes are idempotent, so the redelivery is harmless.
    dropConnection(std::string("ack/nack failed: ") + amqp_error_string2(status));
  }
  return true;
}

void AmqpConsumer::dropConnection(std::string reason) {
  lastError_ = std::move(reason);
  amqp_destroy_connection(asConn(conn_));
  conn_ = nullptr;
  consuming_ = false;
}

void AmqpConsumer::disconnect() {
  if (conn_ == nullptr) {
    return;
  }
  amqp_connection_state_t conn = asConn(conn_);
  amqp_channel_close(conn, kChannel, AMQP_REPLY_SUCCESS);
  amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
  amqp_destroy_connection(conn);
  conn_ = nullptr;
  consuming_ = false;
}

} // namespace rdws::amqp
