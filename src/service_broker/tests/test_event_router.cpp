#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>

#include "Services/EventRouter.h"

using namespace servicegateway;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static rapidjson::Document payload(const char *json)
{
    rapidjson::Document d;
    d.Parse(json);
    return d;
}

static rapidjson::Document emptyPayload()
{
    return payload("{}");
}

static RoutingRule makeRule(const std::string &in, const std::string &out,
                            int priority = 0, bool enabled = true)
{
    RoutingRule r;
    r.inputCapability  = in;
    r.outputCapability = out;
    r.name             = in + "->" + out;
    r.priority         = priority;
    r.enabled          = enabled;
    return r;
}

// ─── Passthrough ─────────────────────────────────────────────────────────────

TEST(EventRouterTest, NoRules_Passthrough)
{
    EventRouter router;
    const auto ep = emptyPayload();
    EXPECT_EQ(router.resolve("echo", ep), "echo");
    EXPECT_EQ(router.resolve("ping", ep), "ping");
}

// ─── Simple rule ─────────────────────────────────────────────────────────────

TEST(EventRouterTest, SimpleRule_MapsCapability)
{
    EventRouter router;
    router.addRule(makeRule("user-create", "user-service-v2"));
    const auto ep = emptyPayload();
    EXPECT_EQ(router.resolve("user-create", ep), "user-service-v2");
    EXPECT_EQ(router.resolve("other",       ep), "other"); // passthrough
}

// ─── Priority ─────────────────────────────────────────────────────────────────

TEST(EventRouterTest, Priority_HigherWins)
{
    EventRouter router;
    router.addRule(makeRule("cap", "low-priority-svc",  0));
    router.addRule(makeRule("cap", "high-priority-svc", 10));
    const auto ep = emptyPayload();
    EXPECT_EQ(router.resolve("cap", ep), "high-priority-svc");
}

// ─── Disabled rule ────────────────────────────────────────────────────────────

TEST(EventRouterTest, DisabledRule_Skipped)
{
    EventRouter router;
    router.addRule(makeRule("cap", "disabled-svc", 10, /*enabled=*/false));
    router.addRule(makeRule("cap", "enabled-svc",   0, /*enabled=*/true));
    const auto ep = emptyPayload();
    EXPECT_EQ(router.resolve("cap", ep), "enabled-svc");
}

// ─── Condition: eq ────────────────────────────────────────────────────────────

TEST(EventRouterTest, Condition_Eq_Matches)
{
    EventRouter router;
    RoutingRule r  = makeRule("api", "admin-svc", 10);
    r.condition    = RouteCondition{"role", "eq", "admin"};
    RoutingRule r2 = makeRule("api", "user-svc", 0);
    router.addRule(std::move(r));
    router.addRule(std::move(r2));

    EXPECT_EQ(router.resolve("api", payload(R"({"role":"admin"})")), "admin-svc");
    EXPECT_EQ(router.resolve("api", payload(R"({"role":"guest"})")), "user-svc");
}

// ─── Condition: exists ────────────────────────────────────────────────────────

TEST(EventRouterTest, Condition_Exists)
{
    EventRouter router;
    RoutingRule r = makeRule("api", "special-svc", 10);
    r.condition   = RouteCondition{"token", "exists", ""};
    RoutingRule fb = makeRule("api", "default-svc", 0);
    router.addRule(std::move(r));
    router.addRule(std::move(fb));

    EXPECT_EQ(router.resolve("api", payload(R"({"token":"abc"})")), "special-svc");
    EXPECT_EQ(router.resolve("api", payload(R"({"other":"x"})")),   "default-svc");
}

// ─── Condition: contains ──────────────────────────────────────────────────────

TEST(EventRouterTest, Condition_Contains)
{
    EventRouter router;
    RoutingRule r = makeRule("search", "image-svc", 5);
    r.condition   = RouteCondition{"query", "contains", "img"};
    RoutingRule fb = makeRule("search", "text-svc", 0);
    router.addRule(std::move(r));
    router.addRule(std::move(fb));

    EXPECT_EQ(router.resolve("search", payload(R"({"query":"find img here"})")), "image-svc");
    EXPECT_EQ(router.resolve("search", payload(R"({"query":"plain text"})")),    "text-svc");
}

// ─── CRUD ─────────────────────────────────────────────────────────────────────

TEST(EventRouterTest, CRUD_AddGetListRemove)
{
    EventRouter router;

    const std::string id = router.addRule(makeRule("a", "b"));
    ASSERT_FALSE(id.empty());

    const auto got = router.getRule(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->inputCapability,  "a");
    EXPECT_EQ(got->outputCapability, "b");
    EXPECT_FALSE(got->createdAt.empty());

    EXPECT_EQ(router.listRules().size(), 1u);

    EXPECT_TRUE(router.removeRule(id));
    EXPECT_FALSE(router.getRule(id).has_value());
    EXPECT_TRUE(router.listRules().empty());
}

TEST(EventRouterTest, Remove_NonExistent_ReturnsFalse)
{
    EventRouter router;
    EXPECT_FALSE(router.removeRule("does-not-exist"));
}

TEST(EventRouterTest, Update_ExistingRule)
{
    EventRouter router;
    const std::string id = router.addRule(makeRule("old-in", "old-out"));

    RoutingRule updated = makeRule("new-in", "new-out", 5);
    ASSERT_TRUE(router.updateRule(id, std::move(updated)));

    const auto got = router.getRule(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id,               id);      // id preserved
    EXPECT_EQ(got->inputCapability,  "new-in");
    EXPECT_EQ(got->outputCapability, "new-out");
    EXPECT_EQ(got->priority,         5);
    EXPECT_FALSE(got->createdAt.empty());      // createdAt preserved
    EXPECT_FALSE(got->updatedAt.empty());
}

TEST(EventRouterTest, Update_NonExistent_ReturnsFalse)
{
    EventRouter router;
    EXPECT_FALSE(router.updateRule("ghost-id", makeRule("x", "y")));
}

// ─── Persistence ─────────────────────────────────────────────────────────────

TEST(EventRouterTest, Persistence_SaveLoad_Roundtrip)
{
    const std::string path = "/tmp/test_event_router_rules.json";
    std::remove(path.c_str()); // clean slate

    std::string id1, id2;
    {
        EventRouter writer;
        RoutingRule r1 = makeRule("cap-a", "svc-a", 10);
        r1.fallbackCapability = "svc-fallback";
        RoutingRule r2 = makeRule("cap-b", "svc-b", 0);
        r2.condition = RouteCondition{"env", "eq", "prod"};

        id1 = writer.addRule(std::move(r1));
        id2 = writer.addRule(std::move(r2));
        ASSERT_TRUE(writer.saveToFile(path));
    }
    {
        EventRouter reader;
        ASSERT_TRUE(reader.loadFromFile(path));
        ASSERT_EQ(reader.listRules().size(), 2u);

        // Rules are sorted by priority descending → id1 (priority 10) first.
        const auto &rules = reader.listRules();
        EXPECT_EQ(rules[0].id,               id1);
        EXPECT_EQ(rules[0].outputCapability, "svc-a");
        EXPECT_TRUE(rules[0].fallbackCapability.has_value());
        EXPECT_EQ(*rules[0].fallbackCapability, "svc-fallback");

        EXPECT_EQ(rules[1].id,               id2);
        ASSERT_TRUE(rules[1].condition.has_value());
        EXPECT_EQ(rules[1].condition->field, "env");
        EXPECT_EQ(rules[1].condition->op,    "eq");
        EXPECT_EQ(rules[1].condition->value, "prod");
    }
    std::remove(path.c_str());
}

TEST(EventRouterTest, AutoPersist_OnAdd)
{
    const std::string path = "/tmp/test_event_router_autopersist.json";
    std::remove(path.c_str());

    {
        EventRouter router(path);
        router.addRule(makeRule("x", "y"));
        EXPECT_TRUE(std::filesystem::exists(path));
    }
    {
        EventRouter reader;
        ASSERT_TRUE(reader.loadFromFile(path));
        EXPECT_EQ(reader.listRules().size(), 1u);
    }
    std::remove(path.c_str());
}
