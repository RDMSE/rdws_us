#include "lambda_event.h"

#include "../../shared/utils/json_helper.h"
#include "../../shared/config/config.h"

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

namespace json = rdws::utils::json;

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

static const std::string& getCachedEnvironment() {
  static const std::string environment = rdws::Config().getEnvironment();
  return environment;
}

HttpRequestInfo::HttpRequestInfo(std::string method, const std::string& path, std::string body)
    : method(std::move(method)), path(path), resource(path), body(std::move(body)) {}

RequestContext::RequestContext()
    : requestId(generateRequestId()), stage(getCachedEnvironment()), protocol("HTTP/1.1"), sourceIp("127.0.0.1"),
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

  httpRequest_.method = json::getString(doc, "httpMethod").value_or("");
  httpRequest_.path = json::getString(doc, "path").value_or("");
  httpRequest_.resource = json::getString(doc, "resource").value_or(httpRequest_.path);
  httpRequest_.body = json::getString(doc, "body").value_or("");
  httpRequest_.isBase64Encoded = json::getBool(doc, "isBase64Encoded").value_or(false);

  if (const auto* headers = json::getObject(doc, "headers"); headers != nullptr) {
    for (auto it = headers->MemberBegin(); it != headers->MemberEnd(); ++it) {
      if (it->value.IsString()) {
        httpRequest_.headers[it->name.GetString()] = it->value.GetString();
      }
    }
  }

  if (const auto* queryParams = json::getObject(doc, "queryStringParameters"); queryParams != nullptr) {
    for (auto it = queryParams->MemberBegin(); it != queryParams->MemberEnd(); ++it) {
      if (it->value.IsString()) {
        httpRequest_.queryStringParameters[it->name.GetString()] = it->value.GetString();
      }
    }
  }

  if (const auto* pathParams = json::getObject(doc, "pathParameters"); pathParams != nullptr) {
    for (auto it = pathParams->MemberBegin(); it != pathParams->MemberEnd(); ++it) {
      if (it->value.IsString()) {
        httpRequest_.pathParameters[it->name.GetString()] = it->value.GetString();
      }
    }
  }

  if (const auto* requestContext = json::getObject(doc, "requestContext"); requestContext != nullptr) {
    requestContext_.requestId =
        json::getString(*requestContext, "requestId").value_or(generateRequestId());
    requestContext_.stage = json::getString(*requestContext, "stage").value_or(getCachedEnvironment());
    requestContext_.httpMethod =
        json::getString(*requestContext, "httpMethod").value_or(httpRequest_.method);
    requestContext_.resourcePath =
        json::getString(*requestContext, "resourcePath").value_or(httpRequest_.resource);
    requestContext_.protocol =
        json::getString(*requestContext, "protocol").value_or("HTTP/1.1");
    requestContext_.sourceIp =
        json::getString(*requestContext, "sourceIp").value_or("127.0.0.1");
    requestContext_.userAgent =
        json::getString(*requestContext, "userAgent").value_or("rdws-gateway/1.0");
    requestContext_.requestTimeEpoch =
        json::getInt64(*requestContext, "requestTimeEpoch")
            .value_or(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count());
  }

  if (const auto stageVars = json::getObject(doc, "stageVariables"); stageVars != nullptr) {
    for (auto it = stageVars->MemberBegin(); it != stageVars->MemberEnd(); ++it) {
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

  const auto stringMapToJson = [&allocator](const std::map<std::string, std::string>& map) {
    json::JsonObj obj(allocator);
    for (const auto& [key, value] : map) {
      obj.setValue(key, rapidjson::Value(value.c_str(), allocator));
    }
    return obj.take();
  };

  rapidjson::Value headers = stringMapToJson(httpRequest_.headers);
  rapidjson::Value queryParams = stringMapToJson(httpRequest_.queryStringParameters);
  rapidjson::Value pathParams = stringMapToJson(httpRequest_.pathParameters);

  rapidjson::Value requestContext = json::JsonObj(allocator)
      .set("requestId", requestContext_.requestId)
      .set("stage", requestContext_.stage)
      .set("httpMethod", requestContext_.httpMethod)
      .set("resourcePath", requestContext_.resourcePath)
      .set("protocol", requestContext_.protocol)
      .set("sourceIp", requestContext_.sourceIp)
      .set("userAgent", requestContext_.userAgent)
      .set("requestTimeEpoch", static_cast<int64_t>(requestContext_.requestTimeEpoch))
      .take();

  rapidjson::Value stageVars = stringMapToJson(stageVariables_);

  rapidjson::Value docValue = json::JsonObj(allocator)
                             .set("httpMethod", httpRequest_.method)
                             .set("path", httpRequest_.path)
                             .set("resource", httpRequest_.resource)
                             .set("body", httpRequest_.body)
                             .set("isBase64Encoded", httpRequest_.isBase64Encoded)
                             .setValue("headers", std::move(headers))
                             .setValue("queryStringParameters", std::move(queryParams))
                             .setValue("pathParameters", std::move(pathParams))
                             .setValue("requestContext", std::move(requestContext))
                             .setValue("stageVariables", std::move(stageVars))
                             .take();
  docValue.Swap(doc);
  return json::docToString(doc);
}

} // namespace rdws::types
