#include "Services/EventBus.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace servicegateway;
using namespace std::chrono_literals;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static rapidjson::Document makeDoc(const char *json)
{
    rapidjson::Document d;
    d.Parse(json);
    return d;
}

// Wait up to `ms` milliseconds for `flag` to become true.
static bool waitFor(const std::atomic<bool> &flag, int ms = 500)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// ─── Fixture ──────────────────────────────────────────────────────────────────

class EventBusTest : public ::testing::Test {
protected:
    EventBus bus;

    void SetUp() override    { bus.start(); }
    void TearDown() override { bus.stop();  }
};

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST_F(EventBusTest, Subscribe_ReceivesPublishedEvent)
{
    std::atomic<bool> received{false};
    bus.subscribe("greet", [&](const std::string &, const rapidjson::Document &) {
        received.store(true);
    });

    bus.publish("greet", makeDoc(R"({"hello":"world"})"));
    EXPECT_TRUE(waitFor(received));
}

TEST_F(EventBusTest, MultipleSubscribers_AllReceive)
{
    std::atomic<int> count{0};

    for (int i = 0; i < 3; ++i) {
        bus.subscribe("ping", [&](const std::string &, const rapidjson::Document &) {
            count.fetch_add(1);
        });
    }

    bus.publish("ping", makeDoc(R"({})"));

    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (count.load() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), 3);
}

TEST_F(EventBusTest, CrossTopicIsolation)
{
    std::atomic<bool> gotA{false}, gotB{false};
    bus.subscribe("topicA", [&](const std::string &, const rapidjson::Document &) { gotA = true; });
    bus.subscribe("topicB", [&](const std::string &, const rapidjson::Document &) { gotB = true; });

    bus.publish("topicA", makeDoc(R"({})"));

    EXPECT_TRUE(waitFor(gotA));
    std::this_thread::sleep_for(50ms); // give topicB handler a chance to fire (should not)
    EXPECT_FALSE(gotB.load());
}

TEST_F(EventBusTest, WildcardSubscription_ReceivesAllTopics)
{
    std::atomic<int> count{0};
    bus.subscribe("*", [&](const std::string &, const rapidjson::Document &) {
        count.fetch_add(1);
    });

    bus.publish("topic1", makeDoc(R"({})"));
    bus.publish("topic2", makeDoc(R"({})"));
    bus.publish("topic3", makeDoc(R"({})"));

    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (count.load() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), 3);
}

TEST_F(EventBusTest, Unsubscribe_NoLongerReceives)
{
    std::atomic<int> count{0};
    const SubscriptionId id = bus.subscribe("evt", [&](const std::string &, const rapidjson::Document &) {
        count.fetch_add(1);
    });

    bus.publish("evt", makeDoc(R"({})"));
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (count.load() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_EQ(count.load(), 1);

    EXPECT_TRUE(bus.unsubscribe(id));
    bus.publish("evt", makeDoc(R"({})"));
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(count.load(), 1); // still 1
}

TEST_F(EventBusTest, Unsubscribe_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(bus.unsubscribe("nonexistent-id"));
}

TEST_F(EventBusTest, Publish_NoSubscribers_NoCrash)
{
    // Should not throw or crash
    EXPECT_NO_THROW(bus.publish("empty", makeDoc(R"({"x":1})")));
    std::this_thread::sleep_for(30ms); // let worker drain
}

TEST_F(EventBusTest, PayloadDelivered_Correctly)
{
    std::atomic<bool> checked{false};
    bus.subscribe("data", [&](const std::string &topic, const rapidjson::Document &payload) {
        EXPECT_EQ(topic, "data");
        ASSERT_TRUE(payload.HasMember("value"));
        EXPECT_EQ(payload["value"].GetInt(), 42);
        checked.store(true);
    });

    bus.publish("data", makeDoc(R"({"value":42})"));
    EXPECT_TRUE(waitFor(checked));
}

TEST_F(EventBusTest, AsyncDelivery_OrderPreserved)
{
    std::vector<int> received;
    std::mutex       mu;
    std::atomic<int> count{0};

    bus.subscribe("seq", [&](const std::string &, const rapidjson::Document &payload) {
        std::lock_guard lock(mu);
        received.push_back(payload["n"].GetInt());
        count.fetch_add(1);
    });

    for (int i = 0; i < 5; ++i) {
        rapidjson::Document d;
        d.SetObject();
        d.AddMember("n", i, d.GetAllocator());
        bus.publish("seq", std::move(d));
    }

    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (count.load() < 5 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    ASSERT_EQ(received.size(), 5u);
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(received[i], i);
}

TEST_F(EventBusTest, Stats_ReflectsSubscriptions)
{
    bus.subscribe("alpha", [](const std::string &, const rapidjson::Document &) {});
    bus.subscribe("alpha", [](const std::string &, const rapidjson::Document &) {});
    bus.subscribe("beta",  [](const std::string &, const rapidjson::Document &) {});

    const rapidjson::Document stats = bus.stats();
    ASSERT_TRUE(stats.HasMember("totalSubscriptions"));
    EXPECT_EQ(stats["totalSubscriptions"].GetInt(), 3);

    ASSERT_TRUE(stats.HasMember("topics"));
    const auto &topics = stats["topics"];
    ASSERT_TRUE(topics.HasMember("alpha"));
    EXPECT_EQ(topics["alpha"].GetInt(), 2);
    ASSERT_TRUE(topics.HasMember("beta"));
    EXPECT_EQ(topics["beta"].GetInt(), 1);
}

TEST_F(EventBusTest, ListTopics_HasSubscribedTopics)
{
    bus.subscribe("x", [](const std::string &, const rapidjson::Document &) {});
    bus.subscribe("y", [](const std::string &, const rapidjson::Document &) {});
    bus.subscribe("*", [](const std::string &, const rapidjson::Document &) {}); // wildcard should not appear

    const auto topics = bus.listTopics();
    EXPECT_EQ(topics.size(), 2u);
    EXPECT_NE(std::find(topics.begin(), topics.end(), "x"), topics.end());
    EXPECT_NE(std::find(topics.begin(), topics.end(), "y"), topics.end());
    EXPECT_EQ(std::find(topics.begin(), topics.end(), "*"), topics.end());
}

TEST_F(EventBusTest, SubscriberCount_IncludesWildcard)
{
    bus.subscribe("cap1", [](const std::string &, const rapidjson::Document &) {});
    bus.subscribe("*",    [](const std::string &, const rapidjson::Document &) {});

    // cap1 has 1 direct + 1 wildcard = 2
    EXPECT_EQ(bus.subscriberCount("cap1"), 2u);
    // cap2 has no direct subscriber, but 1 wildcard
    EXPECT_EQ(bus.subscriberCount("cap2"), 1u);
}

TEST_F(EventBusTest, StopDrainsRemainingEvents)
{
    // Publish many events quickly, then stop. All must be dispatched.
    std::atomic<int> count{0};
    bus.subscribe("drain", [&](const std::string &, const rapidjson::Document &) {
        std::this_thread::sleep_for(1ms); // simulate slow handler
        count.fetch_add(1);
    });

    for (int i = 0; i < 20; ++i)
        bus.publish("drain", makeDoc(R"({})"));

    bus.stop(); // must drain before returning
    EXPECT_EQ(count.load(), 20);

    bus.start(); // re-start for TearDown
}
