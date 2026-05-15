#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rdws::types {

template <typename T>
class ServiceResult {
public:
    static ServiceResult success(T data, int statusCode = 200)
    {
        return ServiceResult(true, std::move(data), "", statusCode);
    }

    static ServiceResult error(std::string errorMessage, int statusCode = 500)
    {
        return ServiceResult(false, std::nullopt, std::move(errorMessage), statusCode);
    }

    bool isSuccess() const { return success_; }
    bool isError() const { return !success_; }
    int getStatusCode() const { return statusCode_; }

    const T &getData() const
    {
        if (!success_ || !data_.has_value()) {
            throw std::runtime_error("ServiceResult has no data");
        }
        return data_.value();
    }

    const std::string &getErrorMessage() const { return errorMessage_; }

private:
    ServiceResult(bool success, std::optional<T> data, std::string errorMessage, int statusCode)
        : success_(success), data_(std::move(data)), errorMessage_(std::move(errorMessage)), statusCode_(statusCode)
    {
    }

    bool success_;
    std::optional<T> data_;
    std::string errorMessage_;
    int statusCode_;
};

struct OperationStatus {
    bool ok = true;
    std::string message;
};

using OperationResult = ServiceResult<OperationStatus>;
using CountResult = ServiceResult<std::uint64_t>;

} // namespace rdws::types
