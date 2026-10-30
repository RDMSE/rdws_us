// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

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

  // Connects, declares the durable queue and puts the channel in confirm mode. Returns false
  // on any failure.
  [[nodiscard]] bool connect();

  // Publishes to the default exchange with the queue name as routing key and waits for the
  // broker's confirm (basic.ack), so true means the broker has the message. A plain publish
  // only writes to the socket: after a broker restart the first one still "succeeds" into
  // the kernel buffer and is lost. On failure the connection is dropped, reconnected and the
  // publish retried once — a message the broker did get before the failure may then arrive
  // twice (the consumer's writes are idempotent). Connects lazily if not connected.
  [[nodiscard]] bool publish(const std::string& body);

  // Same guarantee for many messages at once: true iff the broker confirmed all of them.
  // Publishes in chunks without waiting per message, then waits for the chunk's confirms —
  // the broker batches its disk syncs, so this costs about one sync per chunk instead of one
  // per message (~30 ms each on the QA host: a 24 000-reading pack took 12 minutes one by
  // one, blocking IngestionService's only CoAP thread). On failure the whole batch is
  // retried once on a fresh connection.
  [[nodiscard]] bool publishBatch(const std::vector<std::string>& bodies);

  [[nodiscard]] bool isConnected() const { return conn_ != nullptr; }
  // Why the last publish() failed, for logging.
  [[nodiscard]] const std::string& lastError() const { return lastError_; }

  void disconnect();

private:
  std::string host_;
  uint16_t port_;
  std::string user_;
  std::string password_;
  std::string queueName_;
  void* conn_ = nullptr; // amqp_connection_state_t, opaque to avoid leaking amqp.h here
  std::string lastError_;

  uint64_t nextDeliveryTag_ = 1; // confirm-mode tags count from 1 on each new channel

  [[nodiscard]] bool publishAll(const std::vector<std::string>& bodies);
  [[nodiscard]] bool publishChunk(const std::vector<std::string>& bodies, size_t begin,
                                  size_t end);
  void dropConnection(std::string reason);
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
