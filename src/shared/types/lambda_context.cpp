#include "lambda_context.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace rdws::types {

LambdaContext::LambdaContext(std::string requestId,
                             std::string functionName,
                             std::string functionVersion,
                             const std::chrono::milliseconds timeoutMs,
                             const int memoryLimitMB)
    : requestId_(std::move(requestId)),
      functionName_(std::move(functionName)),
      functionVersion_(std::move(functionVersion)),
      timeoutMs_(timeoutMs),
      startTime_(std::chrono::steady_clock::now()),
      memoryLimitMB_(memoryLimitMB)
{
}

LambdaContext::LambdaContext(const std::string &jsonString)
    : startTime_(std::chrono::steady_clock::now())
{
    rapidjson::Document doc;
    doc.Parse(jsonString.c_str());

    if (doc.HasParseError()) {
        throw std::runtime_error("Invalid JSON in LambdaContext constructor");
    }

    requestId_ = "unknown";
    functionName_ = "unknown";
    functionVersion_ = "1.0";
    timeoutMs_ = std::chrono::milliseconds(30000);
    memoryLimitMB_ = 128;

    if (doc.HasMember("requestId") && doc["requestId"].IsString()) {
        requestId_ = doc["requestId"].GetString();
    }
    if (doc.HasMember("functionName") && doc["functionName"].IsString()) {
        functionName_ = doc["functionName"].GetString();
    }
    if (doc.HasMember("functionVersion") && doc["functionVersion"].IsString()) {
        functionVersion_ = doc["functionVersion"].GetString();
    }
    if (doc.HasMember("timeoutMs") && doc["timeoutMs"].IsInt64()) {
        timeoutMs_ = std::chrono::milliseconds(doc["timeoutMs"].GetInt64());
    }
    if (doc.HasMember("memoryLimitMB") && doc["memoryLimitMB"].IsInt()) {
        memoryLimitMB_ = doc["memoryLimitMB"].GetInt();
    }
}

LambdaContext LambdaContext::fromJson(const std::string &jsonString)
{
    return LambdaContext(jsonString);
}

std::string LambdaContext::toJson() const
{
    rapidjson::Document doc;
    doc.SetObject();
    auto &allocator = doc.GetAllocator();

    doc.AddMember("requestId", rapidjson::Value(requestId_.c_str(), allocator), allocator);
    doc.AddMember("functionName", rapidjson::Value(functionName_.c_str(), allocator), allocator);
    doc.AddMember("functionVersion", rapidjson::Value(functionVersion_.c_str(), allocator), allocator);
    doc.AddMember("timeoutMs", rapidjson::Value(static_cast<int64_t>(timeoutMs_.count())), allocator);
    doc.AddMember("memoryLimitMB", rapidjson::Value(memoryLimitMB_), allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    return buffer.GetString();
}

std::chrono::milliseconds LambdaContext::getRemainingTimeMs() const
{
    const auto elapsed = getElapsedTimeMs();
    if (elapsed >= timeoutMs_) {
        return std::chrono::milliseconds(0);
    }
    return timeoutMs_ - elapsed;
}

bool LambdaContext::isTimeoutImminent(const std::chrono::milliseconds bufferMs) const
{
    return getRemainingTimeMs() <= bufferMs;
}

std::chrono::milliseconds LambdaContext::getElapsedTimeMs() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime_);
}

void LambdaContext::log(const std::string &message, const std::string &level) const
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << "[" << std::put_time(std::gmtime(&timeValue), "%Y-%m-%dT%H:%M:%SZ") << "] "
        << "[" << level << "] "
        << "[" << requestId_ << "] "
        << "[" << functionName_ << "] "
        << message;

    std::cerr << oss.str() << std::endl;
}

} // namespace rdws::types
