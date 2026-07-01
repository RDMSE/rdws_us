#include "lambda_params_helper.h"
#include "json_helper.h"
#include "logger.h"
#include <rapidjson/document.h>

namespace json = rdws::utils::json;

namespace rdws::utils {

static constexpr char kJsonParseError[] = "JSON Parse error";
static constexpr char kLambdaParamsSizeError[] = "Wrong number of arguments";

tl::expected<bool, std::string> LambdaParamsHelper::checkParams(const int argc, char* argv[]) {
  if (argc < 3) {
    return tl::unexpected(kLambdaParamsSizeError);
  }

  const LambdaParams lambdaParams = {
      .eventJson = argv[1],
      .contextJson = argv[2],
  };

  rapidjson::Document doc;
  if (doc.Parse(lambdaParams.eventJson.c_str()).HasParseError()) {
    return tl::unexpected(kJsonParseError);
  }

  doc.SetNull();
  if (doc.Parse(lambdaParams.contextJson.c_str()).HasParseError()) {
    return tl::unexpected(kJsonParseError);
  }

  return true;
}

std::string LambdaParamsHelper::getPathParam(const rapidjson::Document& req,
                                             const std::string& key) {

  if (const auto& pathParams = json::getObject(req, "pathParameters");
        pathParams != nullptr) {
    const auto& keyValue = json::getString(*pathParams, key);
    return keyValue.value_or(std::string{});
  }

  return {};
}

std::string LambdaParamsHelper::getStringQueryParam(const rapidjson::Document& req,
                                                    const std::string& key) {
  if (const auto& queryParams = json::getObject(req, "queryStringParameters");
        queryParams != nullptr) {
    const auto& keyValue = json::getString(*queryParams, key);
    return keyValue.value_or(std::string{});
  }
  return {};
}

} // namespace rdws::utils
