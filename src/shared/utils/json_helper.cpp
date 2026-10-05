#include "json_helper.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

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

std::optional<uint> getUInt(const rapidjson::Value& doc, const std::string& field) {
  if (hasField(doc, field) && doc[field.c_str()].IsUint()) {
    return doc[field.c_str()].GetUint();
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

std::optional<double> getNumber(const rapidjson::Value& doc, const std::string& field) {
  if (hasField(doc, field) && doc[field.c_str()].IsNumber()) {
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

namespace {
const rapidjson::Value* getActorIdentity(const rapidjson::Value& req) {
  const auto* lambdaContext = getObject(req, "lambdaContext");
  return lambdaContext ? getObject(*lambdaContext, "identity") : nullptr;
}
} // namespace

std::optional<std::string> getActorSubject(const rapidjson::Value& req) {
  const auto* identity = getActorIdentity(req);
  if (!identity) {
    return std::nullopt;
  }
  // Prefer the human-readable "username" claim (e.g. JWT payload) over the
  // raw "sub" (often just a numeric user id) when both are available.
  if (const auto* claims = getObject(*identity, "claims"); claims) {
    if (const auto username = getString(*claims, "username")) {
      return username;
    }
  }
  return getString(*identity, "subject");
}

std::string getActorSubjectOrDefault(const rapidjson::Value& req) {
  return getActorSubject(req).value_or("system");
}

bool hasActorIdentity(const rapidjson::Value& req) {
  return getActorIdentity(req) != nullptr;
}

std::optional<std::string> getActorClaim(const rapidjson::Value& req, const std::string& claim) {
  const auto* identity = getActorIdentity(req);
  const auto* claims = identity ? getObject(*identity, "claims") : nullptr;
  return claims ? getString(*claims, claim) : std::nullopt;
}

std::string docToString(const rapidjson::Value& doc) {
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  doc.Accept(w);
  return buf.GetString();
}

} // namespace rdws::utils::json
