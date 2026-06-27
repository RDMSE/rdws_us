#pragma once

#include <map>
#include <rapidjson/document.h>
#include <string>

namespace rdws::types {

struct HttpRequestInfo {
  std::string method;
  std::string path;
  std::string resource;
  std::map<std::string, std::string> headers;
  std::map<std::string, std::string> queryStringParameters;
  std::map<std::string, std::string> pathParameters;
  std::string body;
  bool isBase64Encoded = false;

  HttpRequestInfo() = default;
  HttpRequestInfo(std::string method, const std::string& path, std::string body = "");
};

struct RequestContext {
  std::string requestId;
  std::string stage;
  std::string httpMethod;
  std::string resourcePath;
  std::string protocol;
  std::string sourceIp;
  std::string userAgent;
  int64_t requestTimeEpoch;

  RequestContext();
};

class LambdaEvent {
public:
  LambdaEvent(const std::string& method, const std::string& path, const std::string& body = "");
  explicit LambdaEvent(const std::string& jsonString);
  LambdaEvent(int argc, char* argv[]);

  static LambdaEvent fromJson(const std::string& jsonString);
  [[nodiscard]] std::string toJson() const;

  [[nodiscard]] const std::string& getHttpMethod() const {
    return httpRequest_.method;
  }
  [[nodiscard]] const std::string& getPath() const {
    return httpRequest_.path;
  }
  [[nodiscard]] const std::string& getResource() const {
    return httpRequest_.resource;
  }
  [[nodiscard]] const std::string& getBody() const {
    return httpRequest_.body;
  }
  [[nodiscard]] bool isBase64Encoded() const {
    return httpRequest_.isBase64Encoded;
  }

  [[nodiscard]] const std::map<std::string, std::string>& getHeaders() const {
    return httpRequest_.headers;
  }
  [[nodiscard]] std::string getHeader(const std::string& name) const;
  void setHeader(const std::string& name, const std::string& value);

  [[nodiscard]] const std::map<std::string, std::string>& getQueryStringParameters() const {
    return httpRequest_.queryStringParameters;
  }
  [[nodiscard]] std::string getQueryParameter(const std::string& name) const;
  void setQueryParameter(const std::string& name, const std::string& value);

  [[nodiscard]] const std::map<std::string, std::string>& getPathParameters() const {
    return httpRequest_.pathParameters;
  }
  [[nodiscard]] std::string getPathParameter(const std::string& name) const;
  void setPathParameter(const std::string& name, const std::string& value);

  [[nodiscard]] const RequestContext& getRequestContext() const {
    return requestContext_;
  }
  RequestContext& getRequestContext() {
    return requestContext_;
  }

  [[nodiscard]] const std::map<std::string, std::string>& getStageVariables() const {
    return stageVariables_;
  }
  [[nodiscard]] std::string getStageVariable(const std::string& name) const;
  void setStageVariable(const std::string& name, const std::string& value);

  void setBody(const std::string& body);
  [[nodiscard]] bool hasJsonBody() const;
  const rapidjson::Document& getJsonBody();

  void extractPathParameters(const std::string& pattern);
  void parseQueryString(const std::string& queryString);

  [[nodiscard]] bool isGet() const {
    return httpRequest_.method == "GET";
  }
  [[nodiscard]] bool isPost() const {
    return httpRequest_.method == "POST";
  }
  [[nodiscard]] bool isPut() const {
    return httpRequest_.method == "PUT";
  }
  [[nodiscard]] bool isDelete() const {
    return httpRequest_.method == "DELETE";
  }
  [[nodiscard]] bool isPatch() const {
    return httpRequest_.method == "PATCH";
  }

  [[nodiscard]] bool pathMatches(const std::string& pattern) const;

private:
  HttpRequestInfo httpRequest_;
  RequestContext requestContext_;
  std::map<std::string, std::string> stageVariables_;
  rapidjson::Document jsonBody_;
  bool bodyParsed_ = false;
};

} // namespace rdws::types
