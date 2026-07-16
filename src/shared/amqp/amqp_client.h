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
  using MessageHandler = std::function<bool(const std::string& body)>; // true = ack

  AmqpConsumer(std::string host, uint16_t port, std::string user, std::string password,
              std::string queueName);
  ~AmqpConsumer();

  AmqpConsumer(const AmqpConsumer&) = delete;
  AmqpConsumer& operator=(const AmqpConsumer&) = delete;

  [[nodiscard]] bool connect();

  // Blocks waiting for a single message (up to timeoutMs); if one arrives, calls
  // `handler` and acks iff it returns true. Returns false on timeout or connection
  // error (caller decides whether/how to reconnect) — never throws.
  [[nodiscard]] bool consumeOne(const MessageHandler& handler, unsigned int timeoutMs);

  void disconnect();

private:
  std::string host_;
  uint16_t port_;
  std::string user_;
  std::string password_;
  std::string queueName_;
  void* conn_ = nullptr;
  bool consuming_ = false;
};

} // namespace rdws::amqp
