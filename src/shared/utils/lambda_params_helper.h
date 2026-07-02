#pragma once

#include <string>
#include <tl/expected.hpp>
#include <rapidjson/document.h>

namespace rdws::utils {

struct LambdaParams {
  std::string eventJson;
  std::string contextJson;
};

class LambdaParamsHelper {
public:
  static tl::expected<bool, std::string> checkParams(int argc, char* argv[]);
  static std::string getPathParam(const rapidjson::Document& req, const std::string& key);
  static std::string getStringQueryParam(const rapidjson::Document& req, const std::string& key);
};

} // namespace rdws::utils
