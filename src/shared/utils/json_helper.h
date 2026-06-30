#pragma once
#include <optional>
#include <rapidjson/document.h>
#include <string>

namespace rdws::utils::json {
std::optional<std::string> getString(const rapidjson::Value& doc, const std::string& field);
std::optional<int> getInt(const rapidjson::Value& doc, const std::string& field);
std::optional<int64_t> getInt64(const rapidjson::Value& doc, const std::string& field);
std::optional<bool> getBool(const rapidjson::Value& doc, const std::string& field);
std::optional<double> getDouble(const rapidjson::Value& doc, const std::string& field);
const rapidjson::Value* getObject(const rapidjson::Value& doc, const std::string& field);
const rapidjson::Value* getArray(const rapidjson::Value& doc, const std::string& field);
}; // namespace rdws::utils::json
