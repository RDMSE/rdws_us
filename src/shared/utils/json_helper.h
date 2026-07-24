#pragma once
#include <concepts>
#include <optional>
#include <rapidjson/document.h>
#include <string>
#include <type_traits>

namespace rdws::utils::json {
[[nodiscard]] std::optional<std::string> getString(const rapidjson::Value& doc, const std::string& field);
[[nodiscard]] std::optional<int> getInt(const rapidjson::Value& doc, const std::string& field);
[[nodiscard]] std::optional<uint> getUInt(const rapidjson::Value& doc, const std::string& field);
[[nodiscard]] std::optional<int64_t> getInt64(const rapidjson::Value& doc, const std::string& field);
[[nodiscard]] std::optional<bool> getBool(const rapidjson::Value& doc, const std::string& field);
[[nodiscard]] std::optional<double> getDouble(const rapidjson::Value& doc, const std::string& field);
[[nodiscard]] std::optional<double> getNumber(const rapidjson::Value& doc, const std::string& field);
[[nodiscard]] const rapidjson::Value* getObject(const rapidjson::Value& doc, const std::string& field);
[[nodiscard]] const rapidjson::Value* getArray(const rapidjson::Value& doc, const std::string& field);

[[nodiscard]] std::string docToString(const rapidjson::Value& doc);

// Reads lambdaContext.identity.subject, the actor injected by AuthMiddleware.
// Returns nullopt if any level is absent (e.g. AuthMode::NONE, no identity resolved).
[[nodiscard]] std::optional<std::string> getActorSubject(const rapidjson::Value& req);

// Same as getActorSubject, but falls back to "system" instead of nullopt, so
// updated_by is never left empty on a create()/update() call.
[[nodiscard]] std::string getActorSubjectOrDefault(const rapidjson::Value& req);

template <typename T>
concept JsonSettable =
    std::is_arithmetic_v<std::decay_t<T>> || std::convertible_to<T, std::string> ||
    std::convertible_to<T, const char*> || std::same_as<std::decay_t<T>, class JsonObj>;

class JsonObj {
public:
  explicit JsonObj(rapidjson::Document::AllocatorType& alloc)
      : value_(rapidjson::kObjectType), alloc_(alloc) {}

  template <JsonSettable T> JsonObj& set(const char* key, T&& v) {
    using U = std::decay_t<T>;

    if constexpr (std::same_as<U, JsonObj>) {
      value_.AddMember(rapidjson::StringRef(key), v.value_, alloc_);
    } else if constexpr (std::same_as<U, std::string>) {
      value_.AddMember(rapidjson::StringRef(key), rapidjson::Value(v.c_str(), alloc_), alloc_);
    } else if constexpr (std::convertible_to<U, const char*>) {
      value_.AddMember(rapidjson::StringRef(key), rapidjson::Value(v, alloc_), alloc_);
    } else if constexpr (std::same_as<U, bool>) {
      value_.AddMember(rapidjson::StringRef(key), rapidjson::Value(v), alloc_);
    } else if constexpr (std::is_arithmetic_v<U>) {
      value_.AddMember(rapidjson::StringRef(key), rapidjson::Value(v), alloc_);
    }
    return *this;
  }

  JsonObj& setValue(const char* key, ::rapidjson::Value&& v) {
    value_.AddMember(rapidjson::StringRef(key), v, alloc_);
    return *this;
  }

  JsonObj& setValue(const std::string& key, ::rapidjson::Value&& v) {
    value_.AddMember(rapidjson::Value(key.c_str(), alloc_), v, alloc_);
    return *this;
  }

  JsonObj& setJsonOrString(const char* key, const std::string& raw) {
    if (!raw.empty()) {
      rapidjson::Document parsed;
      if (!parsed.Parse(raw.c_str()).HasParseError()) {
        rapidjson::Value copy;
        copy.CopyFrom(parsed, alloc_);
        value_.AddMember(rapidjson::StringRef(key), copy, alloc_);
        return *this;
      }
    }
    value_.AddMember(rapidjson::StringRef(key), rapidjson::Value(raw.c_str(), alloc_), alloc_);
    return *this;
  }

  rapidjson::Value take() {
    return std::move(value_);
  }

  rapidjson::Document::AllocatorType& allocator() const {
    return alloc_;
  }

private:
  rapidjson::Value value_;
  rapidjson::Document::AllocatorType& alloc_;
};

}; // namespace rdws::utils::json
