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
#include <sstream>
#include <string_view>
#include <utility>

namespace servicegateway {

namespace json = rdws::utils::json;

namespace {

std::string escapePrometheusLabel(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char c : value) {
    if (c == '\\' || c == '"') {
      escaped += '\\';
    }
    if (c == '\n') {
      escaped += "\\n";
      continue;
    }
    escaped += c;
  }
  return escaped;
}

/// Formata as métricas por capability (rdws_shared_utils MetricsTracker) e o resumo do
/// gateway (conexões, serviços registrados) no formato texto de exposição do Prometheus.
/// Reaproveita os dados já expostos em /metrics (JSON) e /status — sem lib nova.
std::string formatPrometheusMetrics(const rapidjson::Document& metrics,
                                     const rapidjson::Document& status) {
  std::ostringstream out;

  out << "# HELP rdws_gateway_requests_total Total de requests por capability\n"
      << "# TYPE rdws_gateway_requests_total counter\n";
  if (const auto* capabilities = json::getArray(metrics, "capabilities")) {
    for (const auto& cap : capabilities->GetArray()) {
      const auto name = json::getString(cap, "capability").value_or("unknown");
      const auto count = json::getInt64(cap, "requestCount").value_or(0);
      out << "rdws_gateway_requests_total{capability=\"" << escapePrometheusLabel(name)
          << "\"} " << count << '\n';
    }
  }

  out << "# HELP rdws_gateway_errors_total Total de erros por capability\n"
      << "# TYPE rdws_gateway_errors_total counter\n";
  if (const auto* capabilities = json::getArray(metrics, "capabilities")) {
    for (const auto& cap : capabilities->GetArray()) {
      const auto name = json::getString(cap, "capability").value_or("unknown");
      const auto count = json::getInt64(cap, "errorCount").value_or(0);
      out << "rdws_gateway_errors_total{capability=\"" << escapePrometheusLabel(name)
          << "\"} " << count << '\n';
    }
  }

  out << "# HELP rdws_gateway_timeouts_total Total de timeouts por capability\n"
      << "# TYPE rdws_gateway_timeouts_total counter\n";
  if (const auto* capabilities = json::getArray(metrics, "capabilities")) {
    for (const auto& cap : capabilities->GetArray()) {
      const auto name = json::getString(cap, "capability").value_or("unknown");
      const auto count = json::getInt64(cap, "timeoutCount").value_or(0);
      out << "rdws_gateway_timeouts_total{capability=\"" << escapePrometheusLabel(name)
          << "\"} " << count << '\n';
    }
  }

  out << "# HELP rdws_gateway_latency_avg_ms Latência média (ms) por capability\n"
      << "# TYPE rdws_gateway_latency_avg_ms gauge\n";
  if (const auto* capabilities = json::getArray(metrics, "capabilities")) {
    for (const auto& cap : capabilities->GetArray()) {
      const auto name = json::getString(cap, "capability").value_or("unknown");
      const auto avg = json::getDouble(cap, "avgLatencyMs").value_or(0.0);
      out << "rdws_gateway_latency_avg_ms{capability=\"" << escapePrometheusLabel(name)
          << "\"} " << avg << '\n';
    }
  }

  out << "# HELP rdws_gateway_latency_p99_ms Latência p99 (ms) por capability\n"
      << "# TYPE rdws_gateway_latency_p99_ms gauge\n";
  if (const auto* capabilities = json::getArray(metrics, "capabilities")) {
    for (const auto& cap : capabilities->GetArray()) {
      const auto name = json::getString(cap, "capability").value_or("unknown");
      const auto p99 = json::getDouble(cap, "p99LatencyMs").value_or(0.0);
      out << "rdws_gateway_latency_p99_ms{capability=\"" << escapePrometheusLabel(name)
          << "\"} " << p99 << '\n';
    }
  }

  out << "# HELP rdws_gateway_active_connections Conexões TCP ativas de backends\n"
      << "# TYPE rdws_gateway_active_connections gauge\n"
      << "rdws_gateway_active_connections " << json::getInt64(status, "activeConnections").value_or(0)
      << '\n';

  out << "# HELP rdws_gateway_pending_requests Requests pendentes (aguardando resposta)\n"
      << "# TYPE rdws_gateway_pending_requests gauge\n"
      << "rdws_gateway_pending_requests " << json::getInt64(status, "trackedRequests").value_or(0)
      << '\n';

  if (const auto registryStatus = status.FindMember("registryStatus");
      registryStatus != status.MemberEnd() && registryStatus->value.IsObject()) {
    out << "# HELP rdws_gateway_services_total Serviços backend registrados\n"
        << "# TYPE rdws_gateway_services_total gauge\n"
        << "rdws_gateway_services_total "
        << json::getInt64(registryStatus->value, "totalServices").value_or(0) << '\n';

    out << "# HELP rdws_gateway_services_healthy Serviços backend saudáveis\n"
        << "# TYPE rdws_gateway_services_healthy gauge\n"
        << "rdws_gateway_services_healthy "
        << json::getInt64(registryStatus->value, "healthyServices").value_or(0) << '\n';
  }

  return out.str();
}

} // namespace

using rdws::types::LambdaContext;
using rdws::types::LambdaEvent;
using rdws::types::ServiceResult;
using rdws::utils::ResponseHelper;
namespace logger = rdws::utils::logger;
namespace json = rdws::utils::json;

namespace {
// Each /invoke handler blocks in waitForResponse() until the capability
// timeout (default 30s) — even after the HTTP client gives up and closes
// the connection (httplib doesn't expose "client still connected" check in a normal
// synchronous handler, only in streamed/chunked responses). Fast retries against a
// stuck service quickly exhaust the default pool (max(8, cores-1)), and new
// connections have no free thread to serve — symptom: gateway "doesn't accept"
// the next request. A much larger pool gives more margin before exhausting; it doesn't
// solve the waste of the thread being stuck itself.
constexpr size_t kHttpThreadPoolSize = 64;
} // namespace

HttpGateway::HttpGateway(ServiceGateway& gateway, int port, std::string host, AuthConfig authConfig)
    : gateway_(gateway), host_(std::move(host)), port_(port), auth_(std::move(authConfig)) {
  server_.new_task_queue = [] { return new httplib::ThreadPool(kHttpThreadPoolSize); };
}

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

    // Bug real (2026-07-09): body inválido era ignorado silenciosamente por
    // documentFromRequest (campos do body simplesmente não eram mesclados no payload),
    // fazendo um JSON malformado parecer "campo obrigatório faltando" em vez de "JSON
    // inválido" — confuso pra quem está chamando a API.
    rapidjson::Document bodyCheck;
    if (!request.body.empty()) {
      bodyCheck.Parse(request.body.c_str());
      if (bodyCheck.HasParseError() || !bodyCheck.IsObject()) {
        response.status = 400;
        response.set_content(
            ResponseHelper::returnError("Invalid JSON body", response.status),
            "application/json"
        );
        return;
      }
    } else {
      bodyCheck.SetObject();
    }

    // Schema validation (Fase 13b): capabilities with a registered schema (write
    // capabilities) are rejected here, before ever reaching the backend service,
    // if a required field is missing or has the wrong type.
    const auto validationErrors = schemaRegistry_.validate(capability, bodyCheck);
    if (!validationErrors.empty()) {
      rapidjson::Document details;
      details.SetArray();
      auto& allocator = details.GetAllocator();
      for (const auto& error : validationErrors) {
        json::JsonObj entry(allocator);
        entry.set("field", error.field).set("message", error.message);
        details.PushBack(entry.take(), allocator);
      }
      response.status = 400;
      response.set_content(
          ResponseHelper::returnError("Validation failed", response.status, &details),
          "application/json"
      );
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
      const int statusCode = 503;
      const std::string message = "No backend service is available for capability: " + capability;
      respond(statusCode, ResponseHelper::returnError(message, statusCode));
      return;
    }

    const PendingRequest result =
        gateway_.waitForResponse(requestId, gateway_.getConfig().timeoutFor(capability));

    if (result.state == RequestState::TIMED_OUT) {
      const int statusCode = 504;
      const auto msg = "Service response timed out";
      respond(statusCode, ResponseHelper::returnError(msg, statusCode));
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
    response.set_content(json::docToString(status), "application/json");
  });

  server_.Get("/metrics", [this](const httplib::Request&, httplib::Response& response) {
    rdws::utils::Profiler profiler("gateway");
    auto t = profiler.scoped("GET /metrics");
    const rapidjson::Document metrics = gateway_.getMetrics();
    response.status = 200;
    response.set_content(json::docToString(metrics), "application/json");
  });

  // Formato texto de exposição do Prometheus — scrape target (ver Plano_Deployment.md §3).
  server_.Get("/metrics/prometheus", [this](const httplib::Request&, httplib::Response& response) {
    rdws::utils::Profiler profiler("gateway");
    auto t = profiler.scoped("GET /metrics/prometheus");
    const rapidjson::Document metrics = gateway_.getMetrics();
    const rapidjson::Document status = gateway_.getGatewayStatus();
    response.status = 200;
    response.set_content(formatPrometheusMetrics(metrics, status), "text/plain; version=0.0.4");
  });

  server_.Get("/health", [this](const httplib::Request&, httplib::Response& response) {
    rdws::utils::Profiler profiler("gateway");
    auto t = profiler.scoped("GET /health");
    const rapidjson::Document health = gateway_.getHealth();
    const bool healthy =
        health.HasMember("status") && std::string(health["status"].GetString()) == "healthy";
    response.status = healthy ? 200 : 503;
    response.set_content(json::docToString(health), "application/json");
  });

  server_.Get("/connections", [this](const httplib::Request&, httplib::Response& response) {
    rdws::utils::Profiler profiler("gateway");
    auto t = profiler.scoped("GET /connections");
    rapidjson::Document connections = gateway_.getConnectionStatus();
    auto& allocator = connections.GetAllocator();

    rapidjson::Value connectionsValue;
    connectionsValue.Swap(connections);

    rapidjson::Value connectionsResult =
        json::JsonObj(allocator)
            .setValue("connections", std::move(connectionsValue))
            .set("activeConnections", static_cast<int>(gateway_.getActiveConnectionCount()))
            .take();
    connectionsResult.Swap(connections);

    response.status = 200;
    response.set_content(json::docToString(connections), "application/json");
  });

  server_.Get(R"(/requests/([^/?]+))",
              [this](const httplib::Request& request, httplib::Response& response) {
                const std::string requestId = request.matches[1];
                const rapidjson::Document status = gateway_.getRequestStatus(requestId);
                const bool found = json::getBool(status, "found").value_or(false);
                response.status = found ? 200 : 404;
                response.set_content(json::docToString(status), "application/json");
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
    response.set_content(json::docToString(doc), "application/json");
  });

  // POST /routes — create a new routing rule
  server_.Post("/routes", [this](const httplib::Request& request, httplib::Response& response) {
    rapidjson::Document body;
    body.Parse(request.body.c_str());
    if (body.HasParseError() || !body.IsObject()) {
      response.status = 400;
      response.set_content(ResponseHelper::returnError("Invalid JSON body", response.status),
                           "application/json");
      return;
    }
    servicegateway::RoutingRule rule = gateway_.getEventRouter().ruleFromJson(body);
    if (rule.inputCapability.empty() || rule.outputCapability.empty()) {
      response.status = 400;
      response.set_content(
          ResponseHelper::returnError("inputCapability and outputCapability are required", response.status),
          "application/json");
      return;
    }
    const std::string id = gateway_.getEventRouter().addRule(std::move(rule));
    const auto created = gateway_.getEventRouter().getRule(id);
    response.status = 201;
    response.set_content(json::docToString(gateway_.getEventRouter().ruleToJson(*created)),
                         "application/json");
  });

  // GET /routes/:id — get a specific routing rule
  server_.Get(R"(/routes/([^/?]+))", [this](const httplib::Request& request,
                                            httplib::Response& response) {
    const std::string id = request.matches[1];
    const auto rule = gateway_.getEventRouter().getRule(id);
    if (!rule) {
      response.status = 404;
      response.set_content(
        ResponseHelper::returnError("Route not found", response.status),
        "application/json"
      );
      return;
    }
    response.status = 200;
    response.set_content(json::docToString(gateway_.getEventRouter().ruleToJson(*rule)),
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
      response.set_content(ResponseHelper::returnError("Invalid JSON body", response.status),
                           "application/json");
      return;
    }
    servicegateway::RoutingRule rule = gateway_.getEventRouter().ruleFromJson(body);
    if (!gateway_.getEventRouter().updateRule(id, std::move(rule))) {
      response.status = 404;
      response.set_content(ResponseHelper::returnError("Route not found", response.status), "application/json");
      return;
    }
    const auto updated = gateway_.getEventRouter().getRule(id);
    response.status = 200;
    response.set_content(json::docToString(gateway_.getEventRouter().ruleToJson(*updated)),
                         "application/json");
  });

  // DELETE /routes/:id — remove a routing rule
  server_.Delete(R"(/routes/([^/?]+))", [this](const httplib::Request& request,
                                               httplib::Response& response) {
    const std::string id = request.matches[1];
    if (!gateway_.getEventRouter().removeRule(id)) {
      response.status = 404;
      response.set_content(ResponseHelper::returnError("Route not found", response.status), "application/json");
      return;
    }
    response.status = 204;
  });

  // ── EventBus endpoints ─────────────────────────────────────────────────────────

  // GET /events — bus stats (topics + subscriber counts)
  server_.Get("/events", [this](const httplib::Request&, httplib::Response& response) {
    rapidjson::Document stats = gateway_.getBus().stats();
    response.set_content(json::docToString(stats), "application/json");
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
                     response.set_content(ResponseHelper::returnError("Invalid JSON body", response.status),
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
                 rapidjson::Value result = json::JsonObj(alloc)
                     .set("published", true)
                     .set("topic", topic)
                     .take();
                 resp.Swap(result);
                 response.status = 202;
                 response.set_content(json::docToString(resp), "application/json");
               });

  // ── GatewayConfig endpoints ───────────────────────────────────────────────────

  // GET /config — full config snapshot
  server_.Get("/config", [this](const httplib::Request&, httplib::Response& response) {
    response.status = 200;
    response.set_content(json::docToString(gateway_.getConfig().toJson()), "application/json");
  });

  // PATCH /config/capabilities/{cap} — update per-capability settings
  server_.Patch(R"(/config/capabilities/([^/?]+))", [this](const httplib::Request& request,
                                                           httplib::Response& response) {
    const std::string cap = request.matches[1];

    if (request.body.empty()) {
      response.status = 400;
      response.set_content(ResponseHelper::returnError("Empty body", response.status), "application/json");
      return;
    }

    rapidjson::Document body;
    body.Parse(request.body.c_str());
    if (body.HasParseError() || !body.IsObject()) {
      response.status = 400;
      response.set_content(ResponseHelper::returnError("Invalid JSON", response.status), "application/json");
      return;
    }

    // Read existing config for this capability and apply partial updates.
    CapabilityConfig cfg = gateway_.getConfig().forCapability(cap);
    cfg.timeoutMs = std::chrono::milliseconds{json::getInt(body, "timeoutMs").value_or(0)};
    if (const auto lb = json::getString(body, "loadBalancing"); lb != std::nullopt) {
      cfg.loadBalancing = GatewayConfig::lbStrategyFromString(*lb);
    }
    cfg.maxConcurrentRequests =
        json::getInt(body, "maxConcurrentRequests").value_or(cfg.maxConcurrentRequests);

    gateway_.getConfig().setCapabilityConfig(cap, cfg);

    response.status = 200;
    response.set_content(json::docToString(gateway_.getConfig().toJson()), "application/json");
  });

  // DELETE /config/capabilities/{cap} — remove override (revert to defaults)
  server_.Delete(R"(/config/capabilities/([^/?]+))", [this](const httplib::Request& request,
                                                            httplib::Response& response) {
    const std::string cap = request.matches[1];
    gateway_.getConfig().removeCapabilityConfig(cap);
    response.status = 200;
    response.set_content(json::docToString(gateway_.getConfig().toJson()), "application/json");
  });

  // GET /config/features — list all feature flags
  server_.Get("/config/features", [this](const httplib::Request&, httplib::Response& response) {
    const rapidjson::Document full = gateway_.getConfig().toJson();
    rapidjson::Document resp;
    resp.SetObject();
    auto& allocator = resp.GetAllocator();

    if (const auto* features = json::getObject(full, "features")) {
      rapidjson::Value featsValue;
      featsValue.CopyFrom(*features, allocator);

      rapidjson::Value result = json::JsonObj(allocator).setValue("features", std::move(featsValue)).take();
      resp.Swap(result);
    }
    response.status = 200;
    response.set_content(json::docToString(resp), "application/json");
  });

  // PUT /config/features/{name} — set a feature flag
  // Body: { "enabled": true }
  server_.Put(R"(/config/features/([^/?]+))",
              [this](const httplib::Request& request, httplib::Response& response) {
                const std::string name = request.matches[1];

                rapidjson::Document body;
                body.Parse(request.body.c_str());

                if (body.HasParseError() || !body.IsObject() || !json::getBool(body, "enabled").has_value()) {
                  response.status = 400;
                  response.set_content(
                      ResponseHelper::returnError(R"(Body must be {"enabled": true|false})", 400),
                      "application/json");
                  return;

                }

                const auto enabled = json::getBool(body, "enabled").value();
                gateway_.getConfig().setFeature(name, enabled);

                rapidjson::Document resp;
                resp.SetObject();
                auto& alloc = resp.GetAllocator();
                rapidjson::Value result = json::JsonObj(alloc)
                    .set("feature", name)
                    .set("enabled", enabled)
                    .take();
                resp.Swap(result);
                response.status = 200;
                response.set_content(json::docToString(resp), "application/json");
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
      const auto msg = "No route found for " + request.method + " " + request.path;
      response.set_content(ResponseHelper::returnError(msg, response.status), "application/json");
      return;
    }

    const std::string capability = match->capability;

    // Body inválido ignorado silenciosamente sem isso — ver comentário no handler de
    // /invoke/:capability.
    if (!request.body.empty()) {
      rapidjson::Document bodyCheck;
      bodyCheck.Parse(request.body.c_str());
      if (bodyCheck.HasParseError() || !bodyCheck.IsObject()) {
        response.status = 400;
        response.set_content(ResponseHelper::returnError("Invalid JSON body", response.status),
                             "application/json");
        return;
      }
    }

    // Build event document with path params injected
    rapidjson::Document eventDocument = documentFromRequest(request, capability);
    auto& alloc = eventDocument.GetAllocator();

    // Merge extracted path params into pathParameters
    if (!match->pathParams.empty()) {
      auto member = eventDocument.FindMember("pathParameters");
      if (member == eventDocument.MemberEnd()) {
        eventDocument.AddMember("pathParameters", rapidjson::Value(rapidjson::kObjectType), alloc);
        member = eventDocument.FindMember("pathParameters");
      }
      auto& pp = member->value;
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
      const auto responseCode = 503;
      const auto msg = "No backend service available for capability: " + capability;
      respond(responseCode, ResponseHelper::returnError(msg, responseCode));
      return;
    }

    const PendingRequest result =
        gateway_.waitForResponse(requestId, gateway_.getConfig().timeoutFor(capability));

    if (result.state == RequestState::TIMED_OUT) {
      const auto responseCode = 504;
      const auto msg = "Service response timed out";
      respond(responseCode, ResponseHelper::returnError(msg, responseCode));
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
  server_.Patch(
      R"(/(?!invoke|status|metrics|health|connections|requests|routes|config|features).*)",
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
  json::JsonObj obj(allocator);

  if (!event.getBody().empty()) {
    rapidjson::Document bodyDocument;
    bodyDocument.Parse(event.getBody().c_str());

    if (!bodyDocument.HasParseError() && bodyDocument.IsObject()) {
      for (auto it = bodyDocument.MemberBegin(); it != bodyDocument.MemberEnd(); ++it) {
        rapidjson::Value value;
        value.CopyFrom(it->value, allocator);
        obj.setValue(std::string(it->name.GetString()), std::move(value));
      }
    }
  }

  rapidjson::Value eventValue;
  eventValue.CopyFrom(eventDocument, allocator);

  json::JsonObj headers(allocator);
  for (const auto& [fst, snd] : event.getHeaders()) {
    headers.setValue(fst, rapidjson::Value(snd.c_str(), allocator));
  }

  json::JsonObj queryParams(allocator);
  for (const auto& [fst, snd] : event.getQueryStringParameters()) {
    queryParams.setValue(fst, rapidjson::Value(snd.c_str(), allocator));
  }

  json::JsonObj pathParams(allocator);
  for (const auto& [fst, snd] : event.getPathParameters()) {
    pathParams.setValue(fst, rapidjson::Value(snd.c_str(), allocator));
  }

  const LambdaContext context("http-" + capability, "http-gateway");
  rapidjson::Value contextValue = json::JsonObj(allocator)
                                       .set("requestId", context.getRequestId())
                                       .set("functionName", context.getFunctionName())
                                       .set("functionVersion", context.getFunctionVersion())
                                       .set("timeoutMs", static_cast<int>(context.getTimeout().count()))
                                       .set("memoryLimitMB", context.getMemoryLimitMB())
                                       .take();

  obj.setValue("lambdaEvent", std::move(eventValue))
      .set("httpMethod", event.getHttpMethod())
      .set("path", event.getPath())
      .set("resource", event.getResource())
      .set("body", event.getBody())
      .set("capability", capability)
      .setValue("headers", headers.take())
      .setValue("queryStringParameters", queryParams.take())
      .setValue("pathParameters", pathParams.take())
      .setValue("lambdaContext", std::move(contextValue));

  rapidjson::Value result = obj.take();
  payload.Swap(result);
  return payload;
}

rapidjson::Document HttpGateway::buildAcceptedResponse(const std::string& capability,
                                                       const std::string& requestId,
                                                       const rapidjson::Document& event) {
  rapidjson::Document response;
  response.SetObject();
  auto& allocator = response.GetAllocator();

  rapidjson::Value eventValue;
  eventValue.CopyFrom(event, allocator);

  rapidjson::Value responseValue = json::JsonObj(allocator)
      .set("accepted", true)
      .set("status", "queued")
      .set("requestId", requestId)
      .set("capability", capability)
      .setValue("event", std::move(eventValue))
      .take();

  response.Swap(responseValue);

  return response;
}

std::string HttpGateway::responseDocumentToBody(const rapidjson::Document& document) {
  return json::docToString(document);
}

} // namespace servicegateway
