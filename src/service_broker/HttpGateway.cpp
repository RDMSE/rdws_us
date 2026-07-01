#include "HttpGateway.h"

#include "../shared/types/lambda_context.h"
#include "../shared/types/lambda_event.h"
#include "../shared/types/service_result.h"
#include "../shared/utils/json_helper.h"
#include "../shared/utils/logger.h"
#include "../shared/utils/profiler.h"
#include "../shared/utils/response_helper.h"
#include "Config/GatewayConfig.h"

#include <chrono>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string_view>
#include <utility>

namespace servicegateway {

using rdws::types::LambdaContext;
using rdws::types::LambdaEvent;
using rdws::types::ServiceResult;
using rdws::utils::ResponseHelper;
namespace logger = rdws::utils::logger;

HttpGateway::HttpGateway(ServiceGateway& gateway, int port, std::string host, AuthConfig authConfig)
    : gateway_(gateway), host_(std::move(host)), port_(port), auth_(std::move(authConfig)) {}

bool HttpGateway::start() {
  if (running_.load()) {
    return true;
  }

  registerRoutes();
  running_.store(true);

  serverThread_ = std::thread([this]() {
    logger::info("HTTP gateway listening", "http://" + host_ + ":" + std::to_string(port_));
    server_.listen(host_, port_);
    running_.store(false);
  });

  return true;
}

void HttpGateway::stop() {
  if (!running_.load()) {
    return;
  }

  running_.store(false);
  server_.stop();

  if (serverThread_.joinable()) {
    serverThread_.join();
  }
}

void HttpGateway::registerRoutes() {
  // POST /invoke/:capability — body is fully available in Post() handlers
  server_.Post(R"(/invoke/([^/?]+))", [this](const httplib::Request& request,
                                             httplib::Response& response) {
    const std::string capability = request.matches[1];
    const auto t0 = std::chrono::steady_clock::now();

    // ── Auth check ────────────────────────────────────────────────────
    AuthHttpRequest authReq;
    authReq.path = request.path;
    for (const auto& [k, v] : request.headers) {
      authReq.headers.emplace(k, v);
    }

    const auto [authorized, statusCode, message, identity] = auth_.authenticate(authReq);
    if (!authorized) {
      response.status = statusCode;
      response.set_header("WWW-Authenticate", R"(Bearer realm="rdws-gateway")");
      response.set_content(ResponseHelper::returnError(message, statusCode), "application/json");
      return;
    }

    rapidjson::Document eventDocument = documentFromRequest(request, capability);
    if (identity) {
      AuthMiddleware::injectIdentity(*identity, eventDocument);
    }

    const std::string requestId = gateway_.sendRequest(capability, eventDocument);

    logger::info("http_request", (requestId.empty() ? "-" : requestId) + " " + request.method + " " + capability + " " + request.path);

    auto respond = [&](int status, const std::string& body) {
      const auto latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
      logger::info("http_response", (requestId.empty() ? "-" : requestId) + " " + capability + " status=" + std::to_string(status) + " latency=" + std::to_string(latencyMs) + "ms");
      gateway_.recordMetric(capability, static_cast<double>(latencyMs), status < 400,
                            status == 504);
      response.status = status;
      response.set_content(body, "application/json");
    };

    if (requestId.empty()) {
      respond(503, ResponseHelper::returnError(
                       "No backend service is available for capability: " + capability, 503));
      return;
    }

    const PendingRequest result =
        gateway_.waitForResponse(requestId, gateway_.getConfig().timeoutFor(capability));

    if (result.state == RequestState::TIMED_OUT) {
      respond(504, ResponseHelper::returnError("Service response timed out", 504));
      return;
    }

    if (result.state == RequestState::FAILED) {
      const auto msg =
          result.errorMessage.empty() ? "Service returned an error" : result.errorMessage;
      const int code = result.statusCode > 0 ? result.statusCode : 500;
      respond(code, ResponseHelper::returnError(msg, code));
      return;
    }

    respond(200, result.responsePayload);
  });

  server_.Get("/status", [this](const httplib::Request&, httplib::Response& response) {
    rdws::utils::Profiler profiler("gateway");
    auto t = profiler.scoped("GET /status");
    const rapidjson::Document status = gateway_.getGatewayStatus();
    response.status = 200;
    response.set_content(documentToString(status), "application/json");
  });

  server_.Get("/metrics", [this](const httplib::Request&, httplib::Response& response) {
    rdws::utils::Profiler profiler("gateway");
    auto t = profiler.scoped("GET /metrics");
    const rapidjson::Document metrics = gateway_.getMetrics();
    response.status = 200;
    response.set_content(documentToString(metrics), "application/json");
  });

  server_.Get("/health", [this](const httplib::Request&, httplib::Response& response) {
    rdws::utils::Profiler profiler("gateway");
    auto t = profiler.scoped("GET /health");
    const rapidjson::Document health = gateway_.getHealth();
    const bool healthy =
        health.HasMember("status") && std::string(health["status"].GetString()) == "healthy";
    response.status = healthy ? 200 : 503;
    response.set_content(documentToString(health), "application/json");
  });

  server_.Get("/connections", [this](const httplib::Request&, httplib::Response& response) {
    rdws::utils::Profiler profiler("gateway");
    auto t = profiler.scoped("GET /connections");
    rapidjson::Document connections;
    connections.SetObject();
    auto& allocator = connections.GetAllocator();

    rapidjson::Document connectionList = gateway_.getConnectionStatus();
    rapidjson::Value connectionsValue;
    connectionsValue.CopyFrom(connectionList, allocator);
    connections.AddMember("connections", connectionsValue, allocator);
    connections.AddMember("activeConnections",
                          static_cast<int>(gateway_.getActiveConnectionCount()), allocator);

    response.status = 200;
    response.set_content(documentToString(connections), "application/json");
  });

  server_.Get(R"(/requests/([^/?]+))",
              [this](const httplib::Request& request, httplib::Response& response) {
                const std::string requestId = request.matches[1];
                const rapidjson::Document status = gateway_.getRequestStatus(requestId);
                const bool found = rdws::utils::json::getBool(status, "found").value_or(false);
                response.status = found ? 200 : 404;
                response.set_content(documentToString(status), "application/json");
              });

  // ── EventRouter CRUD ─────────────────────────────────────────────────────

  // GET /routes — list all routing rules
  server_.Get("/routes", [this](const httplib::Request&, httplib::Response& response) {
    rdws::utils::Profiler profiler("gateway");
    auto t = profiler.scoped("GET /routes");
    const auto rules = gateway_.getEventRouter().listRules();

    rapidjson::Document doc;
    doc.SetArray();
    auto& alloc = doc.GetAllocator();
    for (const auto& rule : rules) {
      rapidjson::Document rd = gateway_.getEventRouter().ruleToJson(rule);
      rapidjson::Value rv;
      rv.CopyFrom(rd, alloc);
      doc.PushBack(rv, alloc);
    }
    response.status = 200;
    response.set_content(documentToString(doc), "application/json");
  });

  // POST /routes — create a new routing rule
  server_.Post("/routes", [this](const httplib::Request& request, httplib::Response& response) {
    rapidjson::Document body;
    body.Parse(request.body.c_str());
    if (body.HasParseError() || !body.IsObject()) {
      response.status = 400;
      response.set_content(ResponseHelper::returnError("Invalid JSON body", 400),
                           "application/json");
      return;
    }
    servicegateway::RoutingRule rule = gateway_.getEventRouter().ruleFromJson(body);
    if (rule.inputCapability.empty() || rule.outputCapability.empty()) {
      response.status = 400;
      response.set_content(
          ResponseHelper::returnError("inputCapability and outputCapability are required", 400),
          "application/json");
      return;
    }
    const std::string id = gateway_.getEventRouter().addRule(std::move(rule));
    const auto created = gateway_.getEventRouter().getRule(id);
    response.status = 201;
    response.set_content(documentToString(gateway_.getEventRouter().ruleToJson(*created)),
                         "application/json");
  });

  // GET /routes/:id — get a specific routing rule
  server_.Get(R"(/routes/([^/?]+))", [this](const httplib::Request& request,
                                            httplib::Response& response) {
    const std::string id = request.matches[1];
    const auto rule = gateway_.getEventRouter().getRule(id);
    if (!rule) {
      response.status = 404;
      response.set_content(ResponseHelper::returnError("Route not found", 404), "application/json");
      return;
    }
    response.status = 200;
    response.set_content(documentToString(gateway_.getEventRouter().ruleToJson(*rule)),
                         "application/json");
  });

  // PUT /routes/:id — replace a routing rule
  server_.Put(R"(/routes/([^/?]+))", [this](const httplib::Request& request,
                                            httplib::Response& response) {
    const std::string id = request.matches[1];
    rapidjson::Document body;
    body.Parse(request.body.c_str());
    if (body.HasParseError() || !body.IsObject()) {
      response.status = 400;
      response.set_content(ResponseHelper::returnError("Invalid JSON body", 400),
                           "application/json");
      return;
    }
    servicegateway::RoutingRule rule = gateway_.getEventRouter().ruleFromJson(body);
    if (!gateway_.getEventRouter().updateRule(id, std::move(rule))) {
      response.status = 404;
      response.set_content(ResponseHelper::returnError("Route not found", 404), "application/json");
      return;
    }
    const auto updated = gateway_.getEventRouter().getRule(id);
    response.status = 200;
    response.set_content(documentToString(gateway_.getEventRouter().ruleToJson(*updated)),
                         "application/json");
  });

  // DELETE /routes/:id — remove a routing rule
  server_.Delete(R"(/routes/([^/?]+))", [this](const httplib::Request& request,
                                               httplib::Response& response) {
    const std::string id = request.matches[1];
    if (!gateway_.getEventRouter().removeRule(id)) {
      response.status = 404;
      response.set_content(ResponseHelper::returnError("Route not found", 404), "application/json");
      return;
    }
    response.status = 204;
  });

  // ── EventBus endpoints ─────────────────────────────────────────────────────────

  // GET /events — bus stats (topics + subscriber counts)
  server_.Get("/events", [this](const httplib::Request&, httplib::Response& response) {
    rapidjson::Document stats = gateway_.getBus().stats();
    response.set_content(documentToString(stats), "application/json");
  });

  // POST /events/{topic} — publish an event to the bus
  server_.Post(R"(/events/([^/?]+))",
               [this](const httplib::Request& request, httplib::Response& response) {
                 const std::string topic = request.matches[1];

                 rapidjson::Document payload;
                 if (!request.body.empty()) {
                   payload.Parse(request.body.c_str());
                   if (payload.HasParseError() || !payload.IsObject()) {
                     response.status = 400;
                     response.set_content(ResponseHelper::returnError("Invalid JSON body", 400),
                                          "application/json");
                     return;
                   }
                 } else {
                   payload.SetObject();
                 }

                 gateway_.getBus().publish(topic, std::move(payload));

                 rapidjson::Document resp;
                 resp.SetObject();
                 auto& alloc = resp.GetAllocator();
                 resp.AddMember("published", true, alloc);
                 resp.AddMember("topic", rapidjson::Value(topic.c_str(), alloc), alloc);
                 response.status = 202;
                 response.set_content(documentToString(resp), "application/json");
               });

  // ── GatewayConfig endpoints ───────────────────────────────────────────────────

  // GET /config — full config snapshot
  server_.Get("/config", [this](const httplib::Request&, httplib::Response& response) {
    response.status = 200;
    response.set_content(documentToString(gateway_.getConfig().toJson()), "application/json");
  });

  // PATCH /config/capabilities/{cap} — update per-capability settings
  server_.Patch(R"(/config/capabilities/([^/?]+))", [this](const httplib::Request& request,
                                                           httplib::Response& response) {
    const std::string cap = request.matches[1];

    if (request.body.empty()) {
      response.status = 400;
      response.set_content(ResponseHelper::returnError("Empty body", 400), "application/json");
      return;
    }

    rapidjson::Document body;
    body.Parse(request.body.c_str());
    if (body.HasParseError() || !body.IsObject()) {
      response.status = 400;
      response.set_content(ResponseHelper::returnError("Invalid JSON", 400), "application/json");
      return;
    }

    // Read existing config for this capability and apply partial updates.
    CapabilityConfig cfg = gateway_.getConfig().forCapability(cap);
    cfg.timeoutMs = std::chrono::milliseconds{rdws::utils::json::getInt(body, "timeoutMs").value_or(0)};
    if (auto lb = rdws::utils::json::getString(body, "loadBalancing"); lb != std::nullopt) {
      cfg.loadBalancing = GatewayConfig::lbStrategyFromString(*lb);
    }
    cfg.maxConcurrentRequests =
        rdws::utils::json::getInt(body, "maxConcurrentRequests").value_or(cfg.maxConcurrentRequests);

    gateway_.getConfig().setCapabilityConfig(cap, cfg);

    response.status = 200;
    response.set_content(documentToString(gateway_.getConfig().toJson()), "application/json");
  });

  // DELETE /config/capabilities/{cap} — remove override (revert to defaults)
  server_.Delete(R"(/config/capabilities/([^/?]+))", [this](const httplib::Request& request,
                                                            httplib::Response& response) {
    const std::string cap = request.matches[1];
    gateway_.getConfig().removeCapabilityConfig(cap);
    response.status = 200;
    response.set_content(documentToString(gateway_.getConfig().toJson()), "application/json");
  });

  // GET /config/features — list all feature flags
  server_.Get("/config/features", [this](const httplib::Request&, httplib::Response& response) {
    const rapidjson::Document full = gateway_.getConfig().toJson();
    rapidjson::Document resp;
    resp.SetObject();
    if (full.HasMember("features")) {
      rapidjson::Value feats;
      feats.CopyFrom(full["features"], resp.GetAllocator());
      resp.AddMember("features", feats, resp.GetAllocator());
    }
    response.status = 200;
    response.set_content(documentToString(resp), "application/json");
  });

  // PUT /config/features/{name} — set a feature flag
  // Body: { "enabled": true }
  server_.Put(R"(/config/features/([^/?]+))",
              [this](const httplib::Request& request, httplib::Response& response) {
                const std::string name = request.matches[1];

                rapidjson::Document body;
                body.Parse(request.body.c_str());

                if (body.HasParseError() || !body.IsObject() || !rdws::utils::json::getBool(body, "enabled").has_value()) {
                  response.status = 400;
                  response.set_content(
                      ResponseHelper::returnError(R"(Body must be {"enabled": true|false})", 400),
                      "application/json");
                  return;

                }

                const auto enabled = rdws::utils::json::getBool(body, "enabled").value();
                gateway_.getConfig().setFeature(name, enabled);

                rapidjson::Document resp;
                resp.SetObject();
                auto& alloc = resp.GetAllocator();
                resp.AddMember("feature", rapidjson::Value(name.c_str(), alloc), alloc);
                resp.AddMember("enabled", enabled, alloc);
                response.status = 200;
                response.set_content(documentToString(resp), "application/json");
              });

  // ── REST path routing (EventRouter method+path rules) ────────────────────
  // Catch-all handlers registered last so specific routes above take priority.
  // Each handler resolves the capability via EventRouter.resolveFromPath(),
  // injects extracted {param} values into pathParameters, and dispatches.

  auto restHandler = [this](const httplib::Request& request, httplib::Response& response) {
    const auto t0 = std::chrono::steady_clock::now();

    // Auth check
    AuthHttpRequest authReq;
    authReq.path = request.path;
    for (const auto& [k, v] : request.headers) {
      authReq.headers.emplace(k, v);
    }
    const auto [authorized, statusCode, message, identity] = auth_.authenticate(authReq);
    if (!authorized) {
      response.status = statusCode;
      response.set_header("WWW-Authenticate", R"(Bearer realm="rdws-gateway")");
      response.set_content(ResponseHelper::returnError(message, statusCode), "application/json");
      return;
    }

    // Resolve capability from HTTP method + path
    const auto match = gateway_.getEventRouter().resolveFromPath(request.method, request.path);
    if (!match) {
      response.status = 404;
      response.set_content(ResponseHelper::returnError(
                               "No route found for " + request.method + " " + request.path, 404),
                           "application/json");
      return;
    }

    const std::string capability = match->capability;

    // Build event document with path params injected
    rapidjson::Document eventDocument = documentFromRequest(request, capability);
    auto& alloc = eventDocument.GetAllocator();

    // Merge extracted path params into pathParameters
    if (!match->pathParams.empty()) {
      if (rdws::utils::json::getObject(eventDocument, "pathParameters") == nullptr) {
        eventDocument.AddMember("pathParameters", rapidjson::Value(rapidjson::kObjectType), alloc);
      }
      auto& pp = eventDocument["pathParameters"];
      for (const auto& [k, v] : match->pathParams) {
        if (!pp.HasMember(k.c_str())) {
          pp.AddMember(rapidjson::Value(k.c_str(), alloc), rapidjson::Value(v.c_str(), alloc),
                       alloc);
        }
      }
    }

    if (identity) {
      AuthMiddleware::injectIdentity(*identity, eventDocument);
    }

    const std::string requestId = gateway_.sendRequest(capability, eventDocument);
    logger::info("http_request", (requestId.empty() ? "-" : requestId) + " " + request.method + " " + capability + " " + request.path);

    auto respond = [&](int status, const std::string& body) {
      const auto latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
      logger::info("http_response", (requestId.empty() ? "-" : requestId) + " " + capability + " status=" + std::to_string(status) + " latency=" + std::to_string(latencyMs) + "ms");
      gateway_.recordMetric(capability, static_cast<double>(latencyMs), status < 400,
                            status == 504);
      response.status = status;
      response.set_content(body, "application/json");
    };

    if (requestId.empty()) {
      respond(503, ResponseHelper::returnError(
                       "No backend service available for capability: " + capability, 503));
      return;
    }

    const PendingRequest result =
        gateway_.waitForResponse(requestId, gateway_.getConfig().timeoutFor(capability));

    if (result.state == RequestState::TIMED_OUT) {
      respond(504, ResponseHelper::returnError("Service response timed out", 504));
      return;
    }
    if (result.state == RequestState::FAILED) {
      const auto msg =
          result.errorMessage.empty() ? "Service returned an error" : result.errorMessage;
      const int code = result.statusCode > 0 ? result.statusCode : 500;
      respond(code, ResponseHelper::returnError(msg, code));
      return;
    }
    respond(200, result.responsePayload);
  };

  server_.Get(R"(/(?!invoke|status|metrics|health|connections|requests|routes|config|features).*)",
              restHandler);
  server_.Post(R"(/(?!invoke|status|metrics|health|connections|requests|routes|config|features).*)",
               restHandler);
  server_.Put(R"(/(?!invoke|status|metrics|health|connections|requests|routes|config|features).*)",
              restHandler);
  server_.Delete(
      R"(/(?!invoke|status|metrics|health|connections|requests|routes|config|features).*)",
      restHandler);
}

std::optional<std::string> HttpGateway::extractCapability(const std::string& path) {
  static constexpr std::string prefix = "/invoke/";
  if (!path.starts_with(prefix)) {
    return std::nullopt;
  }

  std::string capability = path.substr(prefix.size());
  if (const std::size_t queryPos = capability.find('?'); queryPos != std::string::npos) {
    capability = capability.substr(0, queryPos);
  }

  if (capability.empty()) {
    return std::nullopt;
  }

  return capability;
}

std::optional<std::string> HttpGateway::extractRequestId(const std::string& path) {
  static constexpr std::string prefix = "/requests/";
  if (!path.starts_with(prefix.data())) {
    return std::nullopt;
  }

  std::string requestId = path.substr(prefix.size());
  if (const std::size_t queryPos = requestId.find('?'); queryPos != std::string::npos) {
    requestId = requestId.substr(0, queryPos);
  }

  if (requestId.empty()) {
    return std::nullopt;
  }

  return requestId;
}

std::string HttpGateway::documentToString(const rapidjson::Document& document) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  return buffer.GetString();
}

rapidjson::Document HttpGateway::documentFromRequest(const httplib::Request& request,
                                                     const std::string& capability) {
  LambdaEvent event(request.method, request.path, request.body);

  for (const auto& [fst, snd] : request.headers) {
    event.setHeader(fst, snd);
  }

  if (!request.params.empty()) {
    std::string qs;
    for (const auto& [key, value] : request.params) {
      if (!qs.empty()) {
        qs += '&';
      }
      qs += key + '=' + value;
    }
    event.parseQueryString(qs);
  }

  event.setPathParameter("capability", capability);

  rapidjson::Document eventDocument;
  eventDocument.Parse(event.toJson().c_str());

  rapidjson::Document payload;
  payload.SetObject();
  auto& allocator = payload.GetAllocator();

  if (!event.getBody().empty()) {
    rapidjson::Document bodyDocument;
    bodyDocument.Parse(event.getBody().c_str());

    if (!bodyDocument.HasParseError() && bodyDocument.IsObject()) {
      for (auto it = bodyDocument.MemberBegin(); it != bodyDocument.MemberEnd(); ++it) {
        rapidjson::Value key(it->name.GetString(), allocator);
        rapidjson::Value value;
        value.CopyFrom(it->value, allocator);
        payload.AddMember(key, value, allocator);
      }
    }
  }

  rapidjson::Value eventValue;
  eventValue.CopyFrom(eventDocument, allocator);
  payload.AddMember("lambdaEvent", eventValue, allocator);
  payload.AddMember("httpMethod", rapidjson::Value(event.getHttpMethod().c_str(), allocator),
                    allocator);
  payload.AddMember("path", rapidjson::Value(event.getPath().c_str(), allocator), allocator);
  payload.AddMember("resource", rapidjson::Value(event.getResource().c_str(), allocator),
                    allocator);
  payload.AddMember("body", rapidjson::Value(event.getBody().c_str(), allocator), allocator);
  payload.AddMember("capability", rapidjson::Value(capability.c_str(), allocator), allocator);

  rapidjson::Value headers(rapidjson::kObjectType);
  for (const auto& [fst, snd] : event.getHeaders()) {
    headers.AddMember(rapidjson::Value(fst.c_str(), allocator),
                      rapidjson::Value(snd.c_str(), allocator), allocator);
  }
  payload.AddMember("headers", headers, allocator);

  rapidjson::Value queryParams(rapidjson::kObjectType);
  for (const auto& [fst, snd] : event.getQueryStringParameters()) {
    queryParams.AddMember(rapidjson::Value(fst.c_str(), allocator),
                          rapidjson::Value(snd.c_str(), allocator), allocator);
  }
  payload.AddMember("queryStringParameters", queryParams, allocator);

  rapidjson::Value pathParams(rapidjson::kObjectType);
  for (const auto& [fst, snd] : event.getPathParameters()) {
    pathParams.AddMember(rapidjson::Value(fst.c_str(), allocator),
                         rapidjson::Value(snd.c_str(), allocator), allocator);
  }
  payload.AddMember("pathParameters", pathParams, allocator);

  rapidjson::Document contextDocument;
  contextDocument.SetObject();
  auto& contextAllocator = contextDocument.GetAllocator();
  LambdaContext context("http-" + capability, "http-gateway");
  contextDocument.AddMember("requestId",
                            rapidjson::Value(context.getRequestId().c_str(), contextAllocator),
                            contextAllocator);
  contextDocument.AddMember("functionName",
                            rapidjson::Value(context.getFunctionName().c_str(), contextAllocator),
                            contextAllocator);
  contextDocument.AddMember(
      "functionVersion", rapidjson::Value(context.getFunctionVersion().c_str(), contextAllocator),
      contextAllocator);
  contextDocument.AddMember("timeoutMs", static_cast<int>(context.getTimeout().count()),
                            contextAllocator);
  contextDocument.AddMember("memoryLimitMB", context.getMemoryLimitMB(), contextAllocator);

  rapidjson::Value contextValue;
  contextValue.CopyFrom(contextDocument, allocator);
  payload.AddMember("lambdaContext", contextValue, allocator);

  return payload;
}

rapidjson::Document HttpGateway::buildAcceptedResponse(const std::string& capability,
                                                       const std::string& requestId,
                                                       const rapidjson::Document& event) {
  rapidjson::Document response;
  response.SetObject();
  auto& allocator = response.GetAllocator();

  response.AddMember("accepted", true, allocator);
  response.AddMember("status", rapidjson::Value("queued", allocator), allocator);
  response.AddMember("requestId", rapidjson::Value(requestId.c_str(), allocator), allocator);
  response.AddMember("capability", rapidjson::Value(capability.c_str(), allocator), allocator);

  rapidjson::Value eventValue;
  eventValue.CopyFrom(event, allocator);
  response.AddMember("event", eventValue, allocator);

  return response;
}

std::string HttpGateway::responseDocumentToBody(const rapidjson::Document& document) {
  return documentToString(document);
}

} // namespace servicegateway
