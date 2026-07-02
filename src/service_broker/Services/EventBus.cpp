#include "EventBus.h"

#include "../../shared/utils/json_helper.h"

#include <algorithm>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>

namespace json = rdws::utils::json;

namespace servicegateway {

// ─── Lifecycle ────────────────────────────────────────────────────────────────

EventBus::~EventBus() {
  stop();
}

void EventBus::start() {
  if (running_.exchange(true)) {
    return; // already running
  }
  workerThread_ = std::thread(&EventBus::workerLoop, this);
}

void EventBus::stop() {
  if (!running_.exchange(false)) {
    return; // already stopped
  }
  queueCv_.notify_all();
  if (workerThread_.joinable()) {
    workerThread_.join();
  }
}

// ─── Subscription ─────────────────────────────────────────────────────────────

SubscriptionId EventBus::subscribe(const std::string& topic, EventHandler handler) {
  const std::string id = generateId();
  std::unique_lock lock(subsMutex_);
  subscriptions_.push_back({.id = id, .topic = topic, .handler = std::move(handler)});
  return id;
}

bool EventBus::unsubscribe(const SubscriptionId& id) {
  std::unique_lock lock(subsMutex_);
  const auto before = subscriptions_.size();
  std::erase_if(subscriptions_, [&id](const Subscription& s) { return s.id == id; });
  return subscriptions_.size() < before;
}

// ─── Publish ──────────────────────────────────────────────────────────────────

void EventBus::publish(const std::string& topic, const rapidjson::Document& payload) {
  if (!running_.load()) {
    return;
  }

  {
    std::scoped_lock lock(queueMutex_);
    QueuedEvent ev;
    ev.topic = topic;
    ev.payload.CopyFrom(payload, ev.payload.GetAllocator());
    eventQueue_.push_back(std::move(ev));
  }
  queueCv_.notify_one();
}

// ─── Observability ────────────────────────────────────────────────────────────

std::vector<std::string> EventBus::listTopics() const {
  std::shared_lock lock(subsMutex_);
  std::vector<std::string> topics;
  for (const auto& s : subscriptions_) {
    if (s.topic != "*" && std::find(topics.begin(), topics.end(), s.topic) == topics.end()) {
      topics.push_back(s.topic);
    }
  }
  return topics;
}

size_t EventBus::subscriberCount(const std::string& topic) const {
  std::shared_lock lock(subsMutex_);
  return static_cast<size_t>(std::ranges::count_if(subscriptions_, [&topic](const Subscription& s) {
    return s.topic == topic || s.topic == "*";
  }));
}

rapidjson::Document EventBus::stats() const {
  rapidjson::Document doc;
  doc.SetObject();
  auto& alloc = doc.GetAllocator();
  json::JsonObj obj(alloc);

  std::map<std::string, int> topicCounts;
  {
    std::shared_lock lock(subsMutex_);
    obj.set("totalSubscriptions", static_cast<int>(subscriptions_.size()));
    for (const auto& s : subscriptions_) {
      ++topicCounts[s.topic];
    }
  }

  json::JsonObj topics(alloc);
  for (const auto& [topic, count] : topicCounts) {
    topics.setValue(topic, rapidjson::Value(count));
  }
  obj.setValue("topics", topics.take());

  rapidjson::Value result = obj.take();
  doc.Swap(result);
  return doc;
}

// ─── Worker ───────────────────────────────────────────────────────────────────

void EventBus::workerLoop() {
  while (true) {
    std::unique_lock<std::mutex> lock(queueMutex_);
    queueCv_.wait(lock, [this] { return !eventQueue_.empty() || !running_.load(); });

    // Drain all pending events
    while (!eventQueue_.empty()) {
      auto [topic, payload] = std::move(eventQueue_.front());
      eventQueue_.pop_front();
      lock.unlock();
      dispatch(topic, payload);
      lock.lock();
    }

    if (!running_.load()) {
      break;
    }
  }
}

void EventBus::dispatch(const std::string& topic, const rapidjson::Document& payload) const {
  std::shared_lock lock(subsMutex_);
  for (const auto& s : subscriptions_) {
    if (s.topic == topic || s.topic == "*") {
      try {
        s.handler(topic, payload);
      } catch (...) {
        // Handlers must not throw; swallow silently.
      }
    }
  }
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

std::string EventBus::generateId() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static std::uniform_int_distribution<uint64_t> dist;

  const uint64_t a = dist(rng);
  const uint64_t b = dist(rng);

  std::ostringstream oss;
  oss << std::hex << std::setfill('0') << std::setw(8) << (a >> 32) << '-' << std::setw(4)
      << ((a >> 16) & 0xFFFF) << '-' << std::setw(4) << (0x4000 | (a & 0x0FFF)) << '-'
      << std::setw(4) << (0x8000 | ((b >> 48) & 0x3FFF)) << '-' << std::setw(12)
      << (b & 0x0000FFFFFFFFFFFF);
  return oss.str();
}

} // namespace servicegateway
