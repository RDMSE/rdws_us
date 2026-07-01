#pragma once

#include "json_helper.h"

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <string>
#include <vector>

namespace rdws::utils {

class ResponseHelper {
public:
  static std::string returnSuccess(const std::string& message = "", int statusCode = 200);

  static std::string returnError(const std::string& message, int statusCode = 500,
                                 const ::rapidjson::Value* details = nullptr);

  static ::rapidjson::Document returnErrorDoc(const std::string& message, int statusCode = 500,
                                              const ::rapidjson::Value* details = nullptr);

  static ::rapidjson::Document returnSuccessDoc(int statusCode = 200,
                                                const std::string& message = "");

  static ::rapidjson::Document returnDataDoc(const ::rapidjson::Value& data, int statusCode = 200,
                                             const std::string& message = "");

  template <typename Builder>
  static ::rapidjson::Document returnDataDoc(Builder&& builder, int statusCode = 200,
                                             const std::string& message = "") {
    ::rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();

    json::JsonObj obj(allocator);
    obj.set("success", true).set("statusCode", statusCode);
    if (!message.empty()) {
      obj.set("message", message);
    }

    ::rapidjson::Value data = builder(allocator);
    obj.setValue("data", std::move(data));

    ::rapidjson::Value result = obj.take();
    result.Swap(doc);
    addMetadata(doc, allocator);
    return doc;
  }

  static std::string returnData(const ::rapidjson::Value& data, const std::string& message = "",
                                int statusCode = 200);

  template <typename T>
  static std::string returnEntity(const T& entity, const std::string& entityName,
                                  const std::string& message = "", int statusCode = 200);

  template <typename T>
  static std::string returnEntities(const std::vector<T>& entities, const std::string& entitiesName,
                                    const std::string& message = "", int statusCode = 200);

  static std::string toString(const ::rapidjson::Value& value);

private:
  static void addMetadata(::rapidjson::Document& doc,
                          ::rapidjson::Document::AllocatorType& allocator);

  static std::string documentToString(const ::rapidjson::Document& doc);
};

// Template implementations must be in header
template <typename T>
std::string ResponseHelper::returnEntity(const T& entity, const std::string& entityName,
                                         const std::string& message, int statusCode) {
  ::rapidjson::Document doc;
  doc.SetObject();
  auto& allocator = doc.GetAllocator();

  ::rapidjson::Value entityObj = entity.toJsonValue(allocator);

  json::JsonObj obj(allocator);
  obj.set("success", true).set("statusCode", statusCode);
  if (!message.empty()) {
    obj.set("message", message);
  }
  obj.setValue(entityName, std::move(entityObj));

  ::rapidjson::Value result = obj.take();
  result.Swap(doc);
  addMetadata(doc, allocator);

  return documentToString(doc);
}

template <typename T>
std::string ResponseHelper::returnEntities(const std::vector<T>& entities,
                                           const std::string& entitiesName,
                                           const std::string& message, int statusCode) {
  ::rapidjson::Document doc;
  doc.SetObject();
  auto& allocator = doc.GetAllocator();

  ::rapidjson::Value entitiesArray(::rapidjson::kArrayType);
  for (const auto& entity : entities) {
    ::rapidjson::Value entityObj = entity.toJsonValue(allocator);
    entitiesArray.PushBack(entityObj, allocator);
  }

  json::JsonObj obj(allocator);
  obj.set("success", true).set("statusCode", statusCode);
  if (!message.empty()) {
    obj.set("message", message);
  }
  obj.setValue(entitiesName, std::move(entitiesArray));
  obj.set("total", static_cast<int>(entities.size()));

  ::rapidjson::Value result = obj.take();
  result.Swap(doc);
  addMetadata(doc, allocator);

  return documentToString(doc);
}

} // namespace rdws::utils
