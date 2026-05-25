#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <rapidjson/document.h>

namespace servicegateway {

using EventHandler   = std::function<void(const std::string &topic,
                                          const rapidjson::Document &payload)>;
using SubscriptionId = std::string;

// ─── EventBus ─────────────────────────────────────────────────────────────────
// Thread-safe async pub/sub channel.
// - publish() enqueues and returns immediately (non-blocking).
// - A background worker dispatches to all matching subscribers.
// - Subscribe with a specific topic or "*" to receive every event.
class EventBus {
public:
    EventBus()  = default;
    ~EventBus();

    // Lifecycle (must be called before publish/subscribe)
    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

    // Subscription management
    SubscriptionId subscribe(const std::string &topic, EventHandler handler);
    bool           unsubscribe(const SubscriptionId &id);

    // Async publish — enqueues the event and returns immediately.
    void publish(const std::string &topic, rapidjson::Document payload);

    // Observability
    std::vector<std::string> listTopics()                              const;
    size_t                   subscriberCount(const std::string &topic) const;
    rapidjson::Document      stats()                                   const;

private:
    struct Subscription {
        SubscriptionId id;
        std::string    topic; // "*" = wildcard
        EventHandler   handler;
    };

    struct QueuedEvent {
        std::string         topic;
        rapidjson::Document payload;
    };

    mutable std::shared_mutex subsMutex_;
    std::vector<Subscription> subscriptions_;

    std::mutex              queueMutex_;
    std::condition_variable queueCv_;
    std::deque<QueuedEvent> eventQueue_;
    std::thread             workerThread_;
    std::atomic<bool>       running_{false};

    static std::string generateId();
    void workerLoop();
    void dispatch(const std::string &topic, const rapidjson::Document &payload);
};

} // namespace servicegateway
