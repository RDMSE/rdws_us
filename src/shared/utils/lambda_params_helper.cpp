#include "lambda_params_helper.h"

#include "json_helper.h"

#include <rapidjson/document.h>

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

  const auto& pathParams = rdws::utils::getObject(req, "pathParameters");
  if (pathParams != nullptr) {
    const auto& keyValue = rdws::utils::getString(*pathParams, key);
    return keyValue.value_or(std::string{});
  }

  return {};
}

std::string LambdaParamsHelper::getStringQueryParam(const rapidjson::Document& req,
                                                    const std::string& key) {
  const auto& queryParams = rdws::utils::getObject(req, "queryStringParameters");
  if (queryParams != nullptr) {
    const auto& keyValue = rdws::utils::getString(*queryParams, key);
    return keyValue.value_or(std::string{});
  }
  return {};
}

} // namespace rdws::utils
