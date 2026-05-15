#pragma once

#include <chrono>
#include <string>

namespace rdws::types {

class LambdaContext {
public:
    LambdaContext(std::string requestId,
                  std::string functionName,
                  std::string functionVersion = "1.0",
                  std::chrono::milliseconds timeoutMs = std::chrono::milliseconds(30000),
                  int memoryLimitMB = 128);

    explicit LambdaContext(const std::string &jsonString);

    static LambdaContext fromJson(const std::string &jsonString);
    std::string toJson() const;

    const std::string &getRequestId() const { return requestId_; }
    const std::string &getFunctionName() const { return functionName_; }
    const std::string &getFunctionVersion() const { return functionVersion_; }
    std::chrono::milliseconds getTimeout() const { return timeoutMs_; }
    int getMemoryLimitMB() const { return memoryLimitMB_; }

    std::chrono::milliseconds getRemainingTimeMs() const;
    bool isTimeoutImminent(std::chrono::milliseconds bufferMs = std::chrono::milliseconds(500)) const;
    std::chrono::milliseconds getElapsedTimeMs() const;

    void log(const std::string &message, const std::string &level = "INFO") const;

private:
    std::string requestId_;
    std::string functionName_;
    std::string functionVersion_;
    std::chrono::milliseconds timeoutMs_;
    std::chrono::time_point<std::chrono::steady_clock> startTime_;
    int memoryLimitMB_;
};

} // namespace rdws::types
