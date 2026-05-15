#include "HttpGateway.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <string_view>
#include <utility>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "../shared/types/lambda_context.h"
#include "../shared/types/lambda_event.h"
#include "../shared/types/service_result.h"

namespace servicegateway {

using rdws::types::LambdaContext;
using rdws::types::LambdaEvent;
using rdws::types::ServiceResult;

HttpGateway::HttpGateway(ServiceGateway &gateway, int port, std::string host)
    : gateway_(gateway), host_(std::move(host)), port_(port)
{
}

bool HttpGateway::start()
{
    if (running_.load()) {
        return true;
    }

    registerRoutes();
    running_.store(true);

    serverThread_ = std::thread([this]() {
        std::cout << "HTTP gateway listening on http://" << host_ << ':' << port_ << '\n';
        server_.listen(host_.c_str(), port_);
        running_.store(false);
    });

    return true;
}

void HttpGateway::stop()
{
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    server_.stop();

    if (serverThread_.joinable()) {
        serverThread_.join();
    }
}

void HttpGateway::registerRoutes()
{
    server_.set_pre_routing_handler([this](const httplib::Request &request, httplib::Response &response) {
        const std::string capability = extractCapability(request.path).value_or("");

        if (request.method == "POST" && !capability.empty()) {
            const rapidjson::Document eventDocument = documentFromRequest(request, capability);
            const std::string requestId = gateway_.sendRequest(capability, eventDocument);

            if (requestId.empty()) {
                const auto error = buildErrorResponse("No backend service is available for capability: " + capability,
                                                      503);
                response.status = 503;
                response.set_content(documentToString(error), "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }

            const PendingRequest result = gateway_.waitForResponse(requestId);

            if (result.state == RequestState::TIMED_OUT) {
                const auto error = buildErrorResponse("Service response timed out", 504);
                response.status = 504;
                response.set_content(documentToString(error), "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }

            if (result.state == RequestState::FAILED) {
                const auto error = buildErrorResponse(
                    result.errorMessage.empty() ? "Service returned an error" : result.errorMessage, 500);
                response.status = 500;
                response.set_content(documentToString(error), "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }

            response.status = 200;
            response.set_content(result.responsePayload, "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        if (request.method == "GET" && request.path == "/status") {
            const rapidjson::Document status = gateway_.getBrokerStatus();
            response.status = 200;
            response.set_content(documentToString(status), "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        if (request.method == "GET" && request.path == "/connections") {
            rapidjson::Document connections;
            connections.SetObject();
            auto &allocator = connections.GetAllocator();

            rapidjson::Document connectionList = gateway_.getConnectionStatus();
            rapidjson::Value connectionsValue;
            connectionsValue.CopyFrom(connectionList, allocator);
            connections.AddMember("connections", connectionsValue, allocator);
            connections.AddMember("activeConnections", static_cast<int>(gateway_.getActiveConnectionCount()), allocator);

            response.status = 200;
            response.set_content(documentToString(connections), "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        if (request.method == "GET") {
            const auto requestId = extractRequestId(request.path);
            if (requestId.has_value()) {
                const rapidjson::Document status = gateway_.getRequestStatus(requestId.value());
                const bool found = status.HasMember("found") && status["found"].IsBool() && status["found"].GetBool();
                response.status = found ? 200 : 404;
                response.set_content(documentToString(status), "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });
}

std::optional<std::string> HttpGateway::extractCapability(const std::string &path)
{
    static constexpr std::string_view prefix = "/invoke/";
    if (path.rfind(prefix.data(), 0) != 0) {
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

std::optional<std::string> HttpGateway::extractRequestId(const std::string &path)
{
    static constexpr std::string_view prefix = "/requests/";
    if (path.rfind(prefix.data(), 0) != 0) {
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

std::string HttpGateway::documentToString(const rapidjson::Document &document)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
    return buffer.GetString();
}

rapidjson::Document HttpGateway::documentFromRequest(const httplib::Request &request,
                                                     const std::string &capability)
{
    LambdaEvent event(request.method,
                      request.path,
                      request.body);

    for (const auto &header : request.headers) {
        event.setHeader(header.first, header.second);
    }

    if (const auto queryPos = request.path.find('?'); queryPos != std::string::npos) {
        const std::string queryString = request.path.substr(queryPos + 1);
        event.parseQueryString(queryString);
    }

    event.setPathParameter("capability", capability);

    rapidjson::Document eventDocument;
    eventDocument.Parse(event.toJson().c_str());

    rapidjson::Document payload;
    payload.SetObject();
    auto &allocator = payload.GetAllocator();

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
    payload.AddMember("httpMethod", rapidjson::Value(event.getHttpMethod().c_str(), allocator), allocator);
    payload.AddMember("path", rapidjson::Value(event.getPath().c_str(), allocator), allocator);
    payload.AddMember("resource", rapidjson::Value(event.getResource().c_str(), allocator), allocator);
    payload.AddMember("body", rapidjson::Value(event.getBody().c_str(), allocator), allocator);
    payload.AddMember("capability", rapidjson::Value(capability.c_str(), allocator), allocator);

    rapidjson::Value headers(rapidjson::kObjectType);
    for (const auto &header : event.getHeaders()) {
        headers.AddMember(rapidjson::Value(header.first.c_str(), allocator),
                          rapidjson::Value(header.second.c_str(), allocator),
                          allocator);
    }
    payload.AddMember("headers", headers, allocator);

    rapidjson::Value queryParams(rapidjson::kObjectType);
    for (const auto &param : event.getQueryStringParameters()) {
        queryParams.AddMember(rapidjson::Value(param.first.c_str(), allocator),
                              rapidjson::Value(param.second.c_str(), allocator),
                              allocator);
    }
    payload.AddMember("queryStringParameters", queryParams, allocator);

    rapidjson::Value pathParams(rapidjson::kObjectType);
    for (const auto &param : event.getPathParameters()) {
        pathParams.AddMember(rapidjson::Value(param.first.c_str(), allocator),
                             rapidjson::Value(param.second.c_str(), allocator),
                             allocator);
    }
    payload.AddMember("pathParameters", pathParams, allocator);

    rapidjson::Document contextDocument;
    contextDocument.SetObject();
    auto &contextAllocator = contextDocument.GetAllocator();
    LambdaContext context("http-" + capability, "http-gateway");
    contextDocument.AddMember("requestId", rapidjson::Value(context.getRequestId().c_str(), contextAllocator), contextAllocator);
    contextDocument.AddMember("functionName", rapidjson::Value(context.getFunctionName().c_str(), contextAllocator), contextAllocator);
    contextDocument.AddMember("functionVersion", rapidjson::Value(context.getFunctionVersion().c_str(), contextAllocator), contextAllocator);
    contextDocument.AddMember("timeoutMs", static_cast<int>(context.getTimeout().count()), contextAllocator);
    contextDocument.AddMember("memoryLimitMB", context.getMemoryLimitMB(), contextAllocator);

    rapidjson::Value contextValue;
    contextValue.CopyFrom(contextDocument, allocator);
    payload.AddMember("lambdaContext", contextValue, allocator);

    return payload;
}

rapidjson::Document HttpGateway::buildAcceptedResponse(const std::string &capability,
                                                       const std::string &requestId,
                                                       const rapidjson::Document &event)
{
    rapidjson::Document response;
    response.SetObject();
    auto &allocator = response.GetAllocator();

    response.AddMember("accepted", true, allocator);
    response.AddMember("status", rapidjson::Value("queued", allocator), allocator);
    response.AddMember("requestId", rapidjson::Value(requestId.c_str(), allocator), allocator);
    response.AddMember("capability", rapidjson::Value(capability.c_str(), allocator), allocator);

    rapidjson::Value eventValue;
    eventValue.CopyFrom(event, allocator);
    response.AddMember("event", eventValue, allocator);

    return response;
}

rapidjson::Document HttpGateway::buildErrorResponse(const std::string &message, int statusCode)
{
    rapidjson::Document response;
    response.SetObject();
    auto &allocator = response.GetAllocator();

    response.AddMember("error", true, allocator);
    response.AddMember("statusCode", statusCode, allocator);
    response.AddMember("message", rapidjson::Value(message.c_str(), allocator), allocator);

    return response;
}

std::string HttpGateway::responseDocumentToBody(const rapidjson::Document &document)
{
    return documentToString(document);
}

} // namespace servicegateway