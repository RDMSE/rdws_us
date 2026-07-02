#pragma once

#include "Auth/AuthMiddleware.h"
#include "Services/ServiceGateway.h"

#include <atomic>
#include <httplib.h>
#include <memory>
#include <optional>
#include <rapidjson/document.h>
#include <string>
#include <thread>

namespace servicegateway {

class HttpGateway {
public:
  explicit HttpGateway( ServiceGateway& gateway,
                        int port = 3001,
                        std::string host = "0.0.0.0",
                        AuthConfig authConfig = {});
  HttpGateway() = delete;

  bool start();
  void stop();
  bool isRunning() const {
    return running_.load();
  }

private:
  ServiceGateway& gateway_;
  std::string host_;
  int port_;
  AuthMiddleware auth_;
  std::atomic<bool> running_{false};
  httplib::Server server_;
  std::thread serverThread_;

  void registerRoutes();
  static std::optional<std::string> extractCapability(const std::string& path);
  static std::optional<std::string> extractRequestId(const std::string& path);
  static std::string documentToString(const rapidjson::Document& document);
  static rapidjson::Document documentFromRequest(const httplib::Request& request,
                                                 const std::string& capability);
  static rapidjson::Document buildAcceptedResponse(const std::string& capability,
                                                   const std::string& requestId,
                                                   const rapidjson::Document& event);
  static std::string responseDocumentToBody(const rapidjson::Document& document);
};

} // namespace servicegateway
