#include "amqp_client.h"

#include <rabbitmq-c/amqp.h>
#include <rabbitmq-c/tcp_socket.h>
#include <rabbitmq-c/framing.h>

namespace rdws::amqp {

namespace {

constexpr amqp_channel_t kChannel = 1;
constexpr int kFrameMax = 131072;

amqp_connection_state_t asConn(void* conn) {
  return static_cast<amqp_connection_state_t>(conn);
}

bool rpcOk(amqp_connection_state_t conn) {
  const amqp_rpc_reply_t reply = amqp_get_rpc_reply(conn);
  return reply.reply_type == AMQP_RESPONSE_NORMAL;
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
  amqp_socket_t* socket = amqp_tcp_socket_new(conn);
  if (socket == nullptr || amqp_socket_open(socket, host_.c_str(), port_) != 0) {
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
    amqp_channel_close(conn, kChannel, AMQP_REPLY_SUCCESS);
    amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(conn);
    return false;
  }

  conn_ = conn;
  return true;
}

bool AmqpProducer::publish(const std::string& body) {
  if (conn_ == nullptr) {
    return false;
  }
  amqp_connection_state_t conn = asConn(conn_);
  amqp_basic_properties_t props;
  props._flags = AMQP_BASIC_DELIVERY_MODE_FLAG;
  props.delivery_mode = 2; // persistent

  const int status = amqp_basic_publish(conn, kChannel, amqp_empty_bytes,
                                        amqp_cstring_bytes(queueName_.c_str()),
                                        /*mandatory=*/0, /*immediate=*/0, &props,
                                        amqp_cstring_bytes(body.c_str()));
  return status == 0;
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
  amqp_socket_t* socket = amqp_tcp_socket_new(conn);
  if (socket == nullptr || amqp_socket_open(socket, host_.c_str(), port_) != 0) {
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
    return false; // timeout, or a non-deliver frame — caller just retries
  }

  const std::string body(static_cast<const char*>(envelope.message.body.bytes),
                         envelope.message.body.len);
  const bool ack = handler(body);
  if (ack) {
    amqp_basic_ack(conn, kChannel, envelope.delivery_tag, /*multiple=*/0);
  }
  amqp_destroy_envelope(&envelope);
  return true;
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
