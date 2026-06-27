#include "lambda_event.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace rdws::types {

static std::string generateRequestId() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 15);

  std::string requestId;
  requestId.reserve(36);

  static constexpr char chars[] = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    requestId += chars[dis(gen)];
    if (i == 7 || i == 11 || i == 15 || i == 19) {
      requestId += '-';
    }
  }

  return requestId;
}

HttpRequestInfo::HttpRequestInfo(std::string method, const std::string& path, std::string body)
    : method(std::move(method)), path(path), resource(path), body(std::move(body)) {}

RequestContext::RequestContext()
    : requestId(generateRequestId()), stage("prod"), protocol("HTTP/1.1"), sourceIp("127.0.0.1"),
      userAgent("rdws-gateway/1.0"),
      requestTimeEpoch(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count()) {}

LambdaEvent::LambdaEvent(const std::string& method, const std::string& path,
                         const std::string& body)
    : httpRequest_(method, path, body) {
  requestContext_.httpMethod = method;
  requestContext_.resourcePath = path;

  if (const size_t queryPos = path.find('?'); queryPos != std::string::npos) {
    httpRequest_.path = path.substr(0, queryPos);
    httpRequest_.resource = httpRequest_.path;
    requestContext_.resourcePath = httpRequest_.path;
    parseQueryString(path.substr(queryPos + 1));
  }
}

LambdaEvent::LambdaEvent(const std::string& jsonString) {
  rapidjson::Document doc;
  doc.Parse(jsonString.c_str());

  if (doc.HasParseError()) {
    throw std::runtime_error("Invalid JSON in LambdaEvent constructor");
  }

  if (doc.HasMember("httpMethod") && doc["httpMethod"].IsString()) {
    httpRequest_.method = doc["httpMethod"].GetString();
  }
  if (doc.HasMember("path") && doc["path"].IsString()) {
    httpRequest_.path = doc["path"].GetString();
  }
  if (doc.HasMember("resource") && doc["resource"].IsString()) {
    httpRequest_.resource = doc["resource"].GetString();
  } else {
    httpRequest_.resource = httpRequest_.path;
  }
  if (doc.HasMember("body") && doc["body"].IsString()) {
    httpRequest_.body = doc["body"].GetString();
  }
  if (doc.HasMember("isBase64Encoded") && doc["isBase64Encoded"].IsBool()) {
    httpRequest_.isBase64Encoded = doc["isBase64Encoded"].GetBool();
  }

  if (doc.HasMember("headers") && doc["headers"].IsObject()) {
    for (auto it = doc["headers"].MemberBegin(); it != doc["headers"].MemberEnd(); ++it) {
      if (it->value.IsString()) {
        httpRequest_.headers[it->name.GetString()] = it->value.GetString();
      }
    }
  }

  if (doc.HasMember("queryStringParameters") && doc["queryStringParameters"].IsObject()) {
    for (auto it = doc["queryStringParameters"].MemberBegin();
         it != doc["queryStringParameters"].MemberEnd(); ++it) {
      if (it->value.IsString()) {
        httpRequest_.queryStringParameters[it->name.GetString()] = it->value.GetString();
      }
    }
  }

  if (doc.HasMember("pathParameters") && doc["pathParameters"].IsObject()) {
    for (auto it = doc["pathParameters"].MemberBegin(); it != doc["pathParameters"].MemberEnd();
         ++it) {
      if (it->value.IsString()) {
        httpRequest_.pathParameters[it->name.GetString()] = it->value.GetString();
      }
    }
  }

  if (doc.HasMember("requestContext") && doc["requestContext"].IsObject()) {
    const auto& ctx = doc["requestContext"];
    if (ctx.HasMember("requestId") && ctx["requestId"].IsString()) {
      requestContext_.requestId = ctx["requestId"].GetString();
    }
    if (ctx.HasMember("stage") && ctx["stage"].IsString()) {
      requestContext_.stage = ctx["stage"].GetString();
    }
    if (ctx.HasMember("httpMethod") && ctx["httpMethod"].IsString()) {
      requestContext_.httpMethod = ctx["httpMethod"].GetString();
    }
    if (ctx.HasMember("resourcePath") && ctx["resourcePath"].IsString()) {
      requestContext_.resourcePath = ctx["resourcePath"].GetString();
    }
    if (ctx.HasMember("protocol") && ctx["protocol"].IsString()) {
      requestContext_.protocol = ctx["protocol"].GetString();
    }
    if (ctx.HasMember("sourceIp") && ctx["sourceIp"].IsString()) {
      requestContext_.sourceIp = ctx["sourceIp"].GetString();
    }
    if (ctx.HasMember("userAgent") && ctx["userAgent"].IsString()) {
      requestContext_.userAgent = ctx["userAgent"].GetString();
    }
    if (ctx.HasMember("requestTimeEpoch") && ctx["requestTimeEpoch"].IsInt64()) {
      requestContext_.requestTimeEpoch = ctx["requestTimeEpoch"].GetInt64();
    }
  }

  if (doc.HasMember("stageVariables") && doc["stageVariables"].IsObject()) {
    for (auto it = doc["stageVariables"].MemberBegin(); it != doc["stageVariables"].MemberEnd();
         ++it) {
      if (it->value.IsString()) {
        stageVariables_[it->name.GetString()] = it->value.GetString();
      }
    }
  }
}

LambdaEvent::LambdaEvent(const int argc, char* argv[]) {
  std::string method = "GET";
  std::string path = "/";
  std::string body;

  if (argc > 1) {
    method = argv[1];
  }
  if (argc > 2) {
    path = argv[2];
  }
  if (argc > 3) {
    body = argv[3];
  }

  *this = LambdaEvent(method, path, body);
}

LambdaEvent LambdaEvent::fromJson(const std::string& jsonString) {
  return LambdaEvent(jsonString);
}

std::string LambdaEvent::getHeader(const std::string& name) const {
  const auto it = httpRequest_.headers.find(name);
  return (it != httpRequest_.headers.end()) ? it->second : "";
}

void LambdaEvent::setHeader(const std::string& name, const std::string& value) {
  httpRequest_.headers[name] = value;
}

std::string LambdaEvent::getQueryParameter(const std::string& name) const {
  const auto it = httpRequest_.queryStringParameters.find(name);
  return (it != httpRequest_.queryStringParameters.end()) ? it->second : "";
}

void LambdaEvent::setQueryParameter(const std::string& name, const std::string& value) {
  httpRequest_.queryStringParameters[name] = value;
}

std::string LambdaEvent::getPathParameter(const std::string& name) const {
  const auto it = httpRequest_.pathParameters.find(name);
  return (it != httpRequest_.pathParameters.end()) ? it->second : "";
}

void LambdaEvent::setPathParameter(const std::string& name, const std::string& value) {
  httpRequest_.pathParameters[name] = value;
}

std::string LambdaEvent::getStageVariable(const std::string& name) const {
  const auto it = stageVariables_.find(name);
  return (it != stageVariables_.end()) ? it->second : "";
}

void LambdaEvent::setStageVariable(const std::string& name, const std::string& value) {
  stageVariables_[name] = value;
}

void LambdaEvent::setBody(const std::string& body) {
  httpRequest_.body = body;
  bodyParsed_ = false;
}

bool LambdaEvent::hasJsonBody() const {
  return !httpRequest_.body.empty() &&
         (httpRequest_.body.front() == '{' || httpRequest_.body.front() == '[');
}

const rapidjson::Document& LambdaEvent::getJsonBody() {
  if (!bodyParsed_) {
    jsonBody_.Parse(httpRequest_.body.c_str());
    bodyParsed_ = true;
  }
  return jsonBody_;
}

void LambdaEvent::extractPathParameters(const std::string& pattern) {
  const std::regex paramRegex(R"(\{([^}]+)\})");
  std::string regexPattern = pattern;
  std::vector<std::string> paramNames;

  std::sregex_iterator iter(pattern.begin(), pattern.end(), paramRegex);
  const std::sregex_iterator end;

  size_t offset = 0;
  for (; iter != end; ++iter) {
    const std::smatch& match = *iter;
    paramNames.push_back(match[1].str());

    const size_t pos = static_cast<size_t>(match.position()) - offset;
    regexPattern.replace(pos, static_cast<size_t>(match.length()), "([^/]+)");
    offset += static_cast<size_t>(match.length()) - 7;
  }

  const std::regex pathRegex("^" + regexPattern + "$");
  std::smatch pathMatch;
  if (std::regex_match(httpRequest_.path, pathMatch, pathRegex)) {
    for (size_t i = 0; i < paramNames.size() && (i + 1) < pathMatch.size(); ++i) {
      httpRequest_.pathParameters[paramNames[i]] = pathMatch[i + 1].str();
    }
  }
}

void LambdaEvent::parseQueryString(const std::string& queryString) {
  std::istringstream qs(queryString);
  std::string pair;

  while (std::getline(qs, pair, '&')) {
    if (const size_t eqPos = pair.find('='); eqPos != std::string::npos) {
      httpRequest_.queryStringParameters[pair.substr(0, eqPos)] = pair.substr(eqPos + 1);
    } else {
      httpRequest_.queryStringParameters[pair] = "";
    }
  }
}

bool LambdaEvent::pathMatches(const std::string& pattern) const {
  if (pattern == httpRequest_.path) {
    return true;
  }

  if (pattern.find('*') != std::string::npos) {
    std::string regexPattern = pattern;
    std::ranges::replace(regexPattern, '*', '.');
    regexPattern = "^" + regexPattern + "$";
    return std::regex_match(httpRequest_.path, std::regex(regexPattern));
  }

  if (pattern.find('{') != std::string::npos) {
    const std::regex paramRegex(R"(\{[^}]+\})");
    std::string regexPattern = std::regex_replace(pattern, paramRegex, "[^/]+");
    regexPattern = "^" + regexPattern + "$";
    return std::regex_match(httpRequest_.path, std::regex(regexPattern));
  }

  return false;
}

std::string LambdaEvent::toJson() const {
  rapidjson::Document doc;
  doc.SetObject();
  auto& allocator = doc.GetAllocator();

  doc.AddMember("httpMethod", rapidjson::Value(httpRequest_.method.c_str(), allocator), allocator);
  doc.AddMember("path", rapidjson::Value(httpRequest_.path.c_str(), allocator), allocator);
  doc.AddMember("resource", rapidjson::Value(httpRequest_.resource.c_str(), allocator), allocator);
  doc.AddMember("body", rapidjson::Value(httpRequest_.body.c_str(), allocator), allocator);
  doc.AddMember("isBase64Encoded", rapidjson::Value(httpRequest_.isBase64Encoded), allocator);

  rapidjson::Value headers(rapidjson::kObjectType);
  for (const auto& [key, value] : httpRequest_.headers) {
    headers.AddMember(rapidjson::Value(key.c_str(), allocator),
                      rapidjson::Value(value.c_str(), allocator), allocator);
  }
  doc.AddMember("headers", headers, allocator);

  rapidjson::Value queryParams(rapidjson::kObjectType);
  for (const auto& [key, value] : httpRequest_.queryStringParameters) {
    queryParams.AddMember(rapidjson::Value(key.c_str(), allocator),
                          rapidjson::Value(value.c_str(), allocator), allocator);
  }
  doc.AddMember("queryStringParameters", queryParams, allocator);

  rapidjson::Value pathParams(rapidjson::kObjectType);
  for (const auto& [key, value] : httpRequest_.pathParameters) {
    pathParams.AddMember(rapidjson::Value(key.c_str(), allocator),
                         rapidjson::Value(value.c_str(), allocator), allocator);
  }
  doc.AddMember("pathParameters", pathParams, allocator);

  rapidjson::Value requestContext(rapidjson::kObjectType);
  requestContext.AddMember(
      "requestId", rapidjson::Value(requestContext_.requestId.c_str(), allocator), allocator);
  requestContext.AddMember("stage", rapidjson::Value(requestContext_.stage.c_str(), allocator),
                           allocator);
  requestContext.AddMember(
      "httpMethod", rapidjson::Value(requestContext_.httpMethod.c_str(), allocator), allocator);
  requestContext.AddMember(
      "resourcePath", rapidjson::Value(requestContext_.resourcePath.c_str(), allocator), allocator);
  requestContext.AddMember(
      "protocol", rapidjson::Value(requestContext_.protocol.c_str(), allocator), allocator);
  requestContext.AddMember(
      "sourceIp", rapidjson::Value(requestContext_.sourceIp.c_str(), allocator), allocator);
  requestContext.AddMember(
      "userAgent", rapidjson::Value(requestContext_.userAgent.c_str(), allocator), allocator);
  requestContext.AddMember("requestTimeEpoch", rapidjson::Value(requestContext_.requestTimeEpoch),
                           allocator);
  doc.AddMember("requestContext", requestContext, allocator);

  rapidjson::Value stageVars(rapidjson::kObjectType);
  for (const auto& [key, value] : stageVariables_) {
    stageVars.AddMember(rapidjson::Value(key.c_str(), allocator),
                        rapidjson::Value(value.c_str(), allocator), allocator);
  }
  doc.AddMember("stageVariables", stageVars, allocator);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  return buffer.GetString();
}

} // namespace rdws::types
