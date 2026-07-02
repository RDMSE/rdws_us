#include "response_helper.h"

#include "json_helper.h"

#include <ctime>

namespace rdws::utils {

std::string ResponseHelper::returnSuccess(const std::string& message, const int statusCode) {
  return documentToString(returnSuccessDoc(statusCode, message));
}

::rapidjson::Document ResponseHelper::returnSuccessDoc(int statusCode,
                                                       const std::string& message) {
  ::rapidjson::Document doc;
  doc.SetObject();
  auto& allocator = doc.GetAllocator();

  json::JsonObj obj(allocator);
  obj.set("success", true).set("statusCode", statusCode);
  if (!message.empty()) {
    obj.set("message", message);
  }
  ::rapidjson::Value result = obj.take();
  result.Swap(doc);

  addMetadata(doc, allocator);
  return doc;
}

::rapidjson::Document ResponseHelper::returnDataDoc(const ::rapidjson::Value& data, int statusCode,
                                                    const std::string& message) {
  ::rapidjson::Document doc;
  doc.SetObject();
  auto& allocator = doc.GetAllocator();

  json::JsonObj obj(allocator);
  obj.set("success", true).set("statusCode", statusCode);
  if (!message.empty()) {
    obj.set("message", message);
  }

  ::rapidjson::Value dataCopy;
  dataCopy.CopyFrom(data, allocator);
  obj.setValue("data", std::move(dataCopy));

  ::rapidjson::Value result = obj.take();
  result.Swap(doc);
  addMetadata(doc, allocator);
  return doc;
}

::rapidjson::Document ResponseHelper::returnErrorDoc(const std::string& message, int statusCode,
                                                     const ::rapidjson::Value* details) {
  ::rapidjson::Document doc;
  doc.SetObject();
  auto& allocator = doc.GetAllocator();

  json::JsonObj obj(allocator);
  obj.set("success", false).set("statusCode", statusCode).set("error", message);

  if (details != nullptr) {
    ::rapidjson::Value detailsCopy(*details, allocator);
    obj.setValue("details", std::move(detailsCopy));
  }

  ::rapidjson::Value result = obj.take();
  result.Swap(doc);
  addMetadata(doc, allocator);

  return doc;
}

std::string ResponseHelper::returnError(const std::string& message, const int statusCode,
                                        const ::rapidjson::Value* details) {
  return documentToString(returnErrorDoc(message, statusCode, details));
}

std::string ResponseHelper::returnData(const ::rapidjson::Value& data, const std::string& message,
                                       const int statusCode) {
  return documentToString(returnDataDoc(data, statusCode, message));
}

void ResponseHelper::addMetadata(::rapidjson::Document& doc,
                                 ::rapidjson::Document::AllocatorType& allocator) {
  // TODO verify if this field is needed or not
  // doc.AddMember("source", ::rapidjson::Value("microservice C++ with PostgreSQL", allocator),
  //              allocator);
  ::rapidjson::Value ts;
  ts.SetInt64(static_cast<int64_t>(std::time(nullptr)));
  doc.AddMember("timestamp", ts, allocator);
}

std::string ResponseHelper::toString(const ::rapidjson::Value& value) {
  ::rapidjson::StringBuffer buffer;
  ::rapidjson::Writer<::rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return buffer.GetString();
}

std::string ResponseHelper::documentToString(const ::rapidjson::Document& doc) {
  return toString(doc);
}

} // namespace rdws::utils
