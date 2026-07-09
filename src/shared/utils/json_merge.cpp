#include "json_merge.h"

#include "json_helper.h"

#include <rapidjson/document.h>

namespace rdws::utils::json {

std::string mergePatch(const std::string& base, const std::string& patch) {
  rapidjson::Document baseDoc;
  baseDoc.Parse(base.c_str());
  if (baseDoc.HasParseError() || !baseDoc.IsObject()) {
    baseDoc.SetObject();
  }
  auto& alloc = baseDoc.GetAllocator();

  rapidjson::Document patchDoc;
  patchDoc.Parse(patch.c_str());
  if (patchDoc.HasParseError() || !patchDoc.IsObject()) {
    return docToString(baseDoc);
  }

  for (auto it = patchDoc.MemberBegin(); it != patchDoc.MemberEnd(); ++it) {
    const auto* key = it->name.GetString();
    if (it->value.IsNull()) {
      if (baseDoc.HasMember(key)) {
        baseDoc.RemoveMember(key);
      }
      continue;
    }
    rapidjson::Value valueCopy;
    valueCopy.CopyFrom(it->value, alloc);
    if (baseDoc.HasMember(key)) {
      baseDoc[key] = valueCopy;
    } else {
      baseDoc.AddMember(rapidjson::Value(key, alloc), valueCopy, alloc);
    }
  }

  return docToString(baseDoc);
}

} // namespace rdws::utils::json
