#include "json_helper.h"

namespace rdws::utils::json {

static bool hasField(const rapidjson::Value& doc, const std::string& field) {
  return doc.IsObject() && !field.empty() && doc.HasMember(field.c_str());
}

std::optional<std::string> getString(const rapidjson::Value& doc, const std::string& field) {
  if (hasField(doc, field) && doc[field.c_str()].IsString()) {
    return doc[field.c_str()].GetString();
  }
  return std::nullopt;
}

std::optional<int> getInt(const rapidjson::Value& doc, const std::string& field) {
  if (hasField(doc, field) && doc[field.c_str()].IsInt()) {
    return doc[field.c_str()].GetInt();
  }
  return std::nullopt;
}

std::optional<int64_t> getInt64(const rapidjson::Value& doc, const std::string& field) {
  if (hasField(doc, field) && doc[field.c_str()].IsInt64()) {
    return doc[field.c_str()].GetInt64();
  }
  return std::nullopt;
}

std::optional<bool> getBool(const rapidjson::Value& doc, const std::string& field) {
  if (hasField(doc, field) && doc[field.c_str()].IsBool()) {
    return doc[field.c_str()].GetBool();
  }
  return std::nullopt;
}

std::optional<double> getDouble(const rapidjson::Value& doc, const std::string& field) {
  if (hasField(doc, field) && doc[field.c_str()].IsDouble()) {
    return doc[field.c_str()].GetDouble();
  }
  return std::nullopt;
}

const rapidjson::Value* getObject(const rapidjson::Value& doc, const std::string& field) {
  if (hasField(doc, field) && doc[field.c_str()].IsObject()) {
    return &doc[field.c_str()];
  }
  return nullptr;
}

const rapidjson::Value* getArray(const rapidjson::Value& doc, const std::string& field) {
  if (hasField(doc, field) && doc[field.c_str()].IsArray()) {
    return &doc[field.c_str()];
  }
  return nullptr;
}

} // namespace rdws::utils::json
