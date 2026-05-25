#pragma once

#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include <rapidjson/document.h>

namespace servicegateway {

// ─── Condition ───────────────────────────────────────────────────────────────
// Optional predicate evaluated against the request payload.
// Supported ops: "eq", "ne", "contains", "exists"
struct RouteCondition {
    std::string field;  // top-level JSON field name
    std::string op;     // "eq" | "ne" | "contains" | "exists"
    std::string value;  // expected value (string comparison)
};

// ─── RoutingRule ──────────────────────────────────────────────────────────────
struct RoutingRule {
    std::string id;
    std::string name;
    std::string inputCapability;   // capability arriving from the HTTP client
    std::string outputCapability;  // capability routed to the service
    std::optional<RouteCondition>  condition;           // optional payload predicate
    std::optional<std::string>     fallbackCapability;  // used when outputCapability has no service
    int  priority = 0;     // higher → evaluated first
    bool enabled  = true;
    std::string createdAt;
    std::string updatedAt;
};

// ─── EventRouter ─────────────────────────────────────────────────────────────
// Thread-safe, in-memory routing table with optional JSON persistence.
// All write operations (addRule / updateRule / removeRule) auto-save if a
// persistPath was supplied at construction time.
class EventRouter {
public:
    // persistPath: path to a JSON file for persistence.  Empty → no persistence.
    explicit EventRouter(std::string persistPath = "");

    // ── Resolve ──────────────────────────────────────────────────────────────
    // Returns the outputCapability of the first matching rule (sorted by priority
    // descending).  Returns inputCapability unchanged when no rule matches.
    std::string resolve(const std::string &inputCapability,
                        const rapidjson::Document &payload) const;

    // ── CRUD ─────────────────────────────────────────────────────────────────
    std::string              addRule(RoutingRule rule);
    bool                     updateRule(const std::string &id, RoutingRule rule);
    bool                     removeRule(const std::string &id);
    std::optional<RoutingRule> getRule(const std::string &id) const;
    std::vector<RoutingRule> listRules() const;

    // ── Persistence ──────────────────────────────────────────────────────────
    bool loadFromFile(const std::string &path);
    bool saveToFile(const std::string &path) const;

    // ── Serialisation helpers (used by HttpGateway) ──────────────────────────
    rapidjson::Document ruleToJson(const RoutingRule &rule) const;
    RoutingRule         ruleFromJson(const rapidjson::Value &obj) const;

private:
    std::string persistPath_;
    mutable std::shared_mutex mutex_;
    std::vector<RoutingRule>  rules_;

    static std::string generateId();
    static std::string nowIso();
    static bool        matchesCondition(const RouteCondition &cond,
                                        const rapidjson::Document &payload);
    void sortRules();           // highest priority first (stable)
    void persistIfConfigured() const;
};

} // namespace servicegateway
