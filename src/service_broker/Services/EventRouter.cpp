#include "EventRouter.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>

#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace servicegateway {

// ─── Construction ─────────────────────────────────────────────────────────────

EventRouter::EventRouter(std::string persistPath)
    : persistPath_(std::move(persistPath))
{
    if (!persistPath_.empty()) {
        loadFromFile(persistPath_);
    }
}

// ─── Resolve ─────────────────────────────────────────────────────────────────

std::string EventRouter::resolve(const std::string &inputCapability,
                                 const rapidjson::Document &payload) const
{
    std::shared_lock lock(mutex_);
    for (const auto &rule : rules_) {
        if (!rule.enabled) continue;
        if (rule.inputCapability != inputCapability) continue;
        if (rule.condition && !matchesCondition(*rule.condition, payload)) continue;
        return rule.outputCapability;
    }
    return inputCapability; // passthrough — no rule matched
}

// ─── CRUD ─────────────────────────────────────────────────────────────────────

std::string EventRouter::addRule(RoutingRule rule)
{
    rule.id        = generateId();
    rule.createdAt = nowIso();
    rule.updatedAt = rule.createdAt;
    const std::string id = rule.id;

    {
        std::unique_lock lock(mutex_);
        rules_.push_back(std::move(rule));
        sortRules();
    }

    persistIfConfigured();
    return id;
}

bool EventRouter::updateRule(const std::string &id, RoutingRule rule)
{
    bool found = false;
    {
        std::unique_lock lock(mutex_);
        const auto it = std::find_if(rules_.begin(), rules_.end(),
                                     [&id](const RoutingRule &r) { return r.id == id; });
        if (it != rules_.end()) {
            rule.id        = id;
            rule.createdAt = it->createdAt;
            rule.updatedAt = nowIso();
            *it            = std::move(rule);
            sortRules();
            found = true;
        }
    }

    if (found) persistIfConfigured();
    return found;
}

bool EventRouter::removeRule(const std::string &id)
{
    bool found = false;
    {
        std::unique_lock lock(mutex_);
        const auto it = std::find_if(rules_.begin(), rules_.end(),
                                     [&id](const RoutingRule &r) { return r.id == id; });
        if (it != rules_.end()) {
            rules_.erase(it);
            found = true;
        }
    }

    if (found) persistIfConfigured();
    return found;
}

std::optional<RoutingRule> EventRouter::getRule(const std::string &id) const
{
    std::shared_lock lock(mutex_);
    const auto it = std::find_if(rules_.cbegin(), rules_.cend(),
                                 [&id](const RoutingRule &r) { return r.id == id; });
    if (it == rules_.cend()) return std::nullopt;
    return *it;
}

std::vector<RoutingRule> EventRouter::listRules() const
{
    std::shared_lock lock(mutex_);
    return rules_;
}

// ─── Persistence ─────────────────────────────────────────────────────────────

bool EventRouter::loadFromFile(const std::string &path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;

    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document doc;
    doc.ParseStream(isw);
    if (doc.HasParseError() || !doc.IsArray()) return false;

    std::unique_lock lock(mutex_);
    rules_.clear();
    for (const auto &obj : doc.GetArray()) {
        if (obj.IsObject()) {
            rules_.push_back(ruleFromJson(obj));
        }
    }
    sortRules();
    return true;
}

bool EventRouter::saveToFile(const std::string &path) const
{
    std::ofstream ofs(path);
    if (!ofs.is_open()) return false;

    rapidjson::Document doc;
    doc.SetArray();
    auto &alloc = doc.GetAllocator();

    {
        std::shared_lock lock(mutex_);
        for (const auto &rule : rules_) {
            rapidjson::Document ruleDoc = ruleToJson(rule);
            rapidjson::Value ruleVal;
            ruleVal.CopyFrom(ruleDoc, alloc);
            doc.PushBack(ruleVal, alloc);
        }
    }

    rapidjson::OStreamWrapper osw(ofs);
    rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(osw);
    doc.Accept(writer);
    return true;
}

// ─── Serialisation ────────────────────────────────────────────────────────────

rapidjson::Document EventRouter::ruleToJson(const RoutingRule &rule) const
{
    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();

    doc.AddMember("id",               rapidjson::Value(rule.id.c_str(), a), a);
    doc.AddMember("name",             rapidjson::Value(rule.name.c_str(), a), a);
    doc.AddMember("inputCapability",  rapidjson::Value(rule.inputCapability.c_str(), a), a);
    doc.AddMember("outputCapability", rapidjson::Value(rule.outputCapability.c_str(), a), a);
    doc.AddMember("priority",         rule.priority, a);
    doc.AddMember("enabled",          rule.enabled, a);
    doc.AddMember("createdAt",        rapidjson::Value(rule.createdAt.c_str(), a), a);
    doc.AddMember("updatedAt",        rapidjson::Value(rule.updatedAt.c_str(), a), a);

    if (rule.condition) {
        rapidjson::Value cond(rapidjson::kObjectType);
        cond.AddMember("field", rapidjson::Value(rule.condition->field.c_str(), a), a);
        cond.AddMember("op",    rapidjson::Value(rule.condition->op.c_str(), a), a);
        cond.AddMember("value", rapidjson::Value(rule.condition->value.c_str(), a), a);
        doc.AddMember("condition", cond, a);
    } else {
        doc.AddMember("condition", rapidjson::Value(rapidjson::kNullType), a);
    }

    if (rule.fallbackCapability) {
        doc.AddMember("fallbackCapability",
                      rapidjson::Value(rule.fallbackCapability->c_str(), a), a);
    } else {
        doc.AddMember("fallbackCapability", rapidjson::Value(rapidjson::kNullType), a);
    }

    return doc;
}

RoutingRule EventRouter::ruleFromJson(const rapidjson::Value &obj) const
{
    RoutingRule rule;

    auto str = [&](const char *key, const std::string &def = "") -> std::string {
        if (obj.HasMember(key) && obj[key].IsString()) return obj[key].GetString();
        return def;
    };

    rule.id               = str("id");
    rule.name             = str("name");
    rule.inputCapability  = str("inputCapability");
    rule.outputCapability = str("outputCapability");
    rule.priority         = (obj.HasMember("priority") && obj["priority"].IsInt())
                                ? obj["priority"].GetInt() : 0;
    rule.enabled          = (!obj.HasMember("enabled") || !obj["enabled"].IsBool())
                                ? true : obj["enabled"].GetBool();
    rule.createdAt        = str("createdAt");
    rule.updatedAt        = str("updatedAt");

    if (obj.HasMember("condition") && obj["condition"].IsObject()) {
        const auto &c = obj["condition"];
        RouteCondition cond;
        if (c.HasMember("field") && c["field"].IsString()) cond.field = c["field"].GetString();
        if (c.HasMember("op")    && c["op"].IsString())    cond.op    = c["op"].GetString();
        if (c.HasMember("value") && c["value"].IsString()) cond.value = c["value"].GetString();
        rule.condition = cond;
    }

    if (obj.HasMember("fallbackCapability") && obj["fallbackCapability"].IsString()) {
        rule.fallbackCapability = obj["fallbackCapability"].GetString();
    }

    return rule;
}

// ─── Condition matching ───────────────────────────────────────────────────────

bool EventRouter::matchesCondition(const RouteCondition &cond,
                                   const rapidjson::Document &payload)
{
    if (!payload.IsObject()) return false;

    const bool fieldExists = payload.HasMember(cond.field.c_str());

    if (cond.op == "exists") return fieldExists;
    if (!fieldExists) return false;

    // Coerce field to string for comparison
    const auto &fv = payload[cond.field.c_str()];
    std::string strVal;
    if      (fv.IsString()) strVal = fv.GetString();
    else if (fv.IsBool())   strVal = fv.GetBool() ? "true" : "false";
    else if (fv.IsInt())    strVal = std::to_string(fv.GetInt());
    else if (fv.IsUint())   strVal = std::to_string(fv.GetUint());
    else if (fv.IsDouble()) strVal = std::to_string(fv.GetDouble());

    if (cond.op == "eq")       return strVal == cond.value;
    if (cond.op == "ne")       return strVal != cond.value;
    if (cond.op == "contains") return strVal.find(cond.value) != std::string::npos;
    return false;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

std::string EventRouter::generateId()
{
    static std::mt19937_64 rng{std::random_device{}()};
    static std::uniform_int_distribution<uint32_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(8) << dist(rng) << '-'
        << std::setw(4) << (dist(rng) & 0xFFFFu) << '-'
        << std::setw(4) << ((dist(rng) & 0x0FFFu) | 0x4000u) << '-'
        << std::setw(4) << ((dist(rng) & 0x3FFFu) | 0x8000u) << '-'
        << std::setw(8) << dist(rng)
        << std::setw(4) << (dist(rng) & 0xFFFFu);
    return oss.str();
}

std::string EventRouter::nowIso()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

void EventRouter::sortRules()
{
    std::stable_sort(rules_.begin(), rules_.end(),
                     [](const RoutingRule &a, const RoutingRule &b) {
                         return a.priority > b.priority;
                     });
}

void EventRouter::persistIfConfigured() const
{
    if (!persistPath_.empty()) {
        saveToFile(persistPath_);
    }
}

} // namespace servicegateway
