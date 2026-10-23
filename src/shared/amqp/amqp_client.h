#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rdws::amqp {

// Thin wrappers around rabbitmq-c (AMQP 0-9-1) for the sensor_readings pipeline
// (Plano_Ingestion.md) — one durable queue, one message per reading. Both classes
// own a single connection for their lifetime (not pooled), matching the
// one-service-one-purpose shape of IngestionService (producer) and
// ReadingWriterService (consumer).

class AmqpProducer {
public:
  AmqpProducer(std::string host, uint16_t port, std::string user, std::string password,
              std::string queueName);
  ~AmqpProducer();

  AmqpProducer(const AmqpProducer&) = delete;
  AmqpProducer& operator=(const AmqpProducer&) = delete;

  // Connects and declares the durable queue. Returns false on any failure.
  [[nodiscard]] bool connect();

  // Publishes to the default exchange with the queue name as routing key.
  [[nodiscard]] bool publish(const std::string& body);

  void disconnect();

private:
  std::string host_;
  uint16_t port_;
  std::string user_;
  std::string password_;
  std::string queueName_;
  void* conn_ = nullptr; // amqp_connection_state_t, opaque to avoid leaking amqp.h here
};

class AmqpConsumer {
public:
  // What to do with a delivery once the handler is done with it.
  enum class Outcome {
    Ack,     // processed (or discarded on purpose) — remove it from the queue
    Requeue, // not processed — basic_nack with requeue, the broker delivers it again
  };
  using MessageHandler = std::function<Outcome(const std::string& body)>;

  AmqpConsumer(std::string host, uint16_t port, std::string user, std::string password,
              std::string queueName);
  ~AmqpConsumer();

  AmqpConsumer(const AmqpConsumer&) = delete;
  AmqpConsumer& operator=(const AmqpConsumer&) = delete;

  [[nodiscard]] bool connect();

  // Blocks waiting for a single message (up to timeoutMs); if one arrives, calls
  // `handler` and acks or nacks (with requeue) according to its Outcome. Returns false on
  // timeout or connection error — never throws. Anything other than a timeout (socket
  // error, broker closing the connection or channel, consumer cancelled) drops the
  // connection: isConnected() turns false and the caller reconnects with connect().
  [[nodiscard]] bool consumeOne(const MessageHandler& handler, unsigned int timeoutMs);

  [[nodiscard]] bool isConnected() const { return conn_ != nullptr; }
  [[nodiscard]] const std::string& queueName() const { return queueName_; }
  // Why the connection was last dropped by consumeOne(), for logging.
  [[nodiscard]] const std::string& lastError() const { return lastError_; }

  void disconnect();

private:
  std::string host_;
  uint16_t port_;
  std::string user_;
  std::string password_;
  std::string queueName_;
  void* conn_ = nullptr;
  bool consuming_ = false;
  std::string lastError_;

  // Tears down a connection already known to be dead: no close handshake, which would
  // wait on a socket that won't answer.
  void dropConnection(std::string reason);
};

} // namespace rdws::amqp
