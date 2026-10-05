#include "senml.h"

#include <rapidjson/document.h>

#include <cctype>
#include <string_view>

namespace rdws::senml {

namespace {

// RFC 8428 §4.5.3: times below 2^28 are relative to the current time.
constexpr double kRelativeTimeLimit = 268435456.0;

// Highest SenML version this parser understands (RFC 8428 is version 10).
constexpr int kMaxVersion = 10;

// Fields this parser understands. Anything else is ignored, unless it ends in '_'.
bool isKnownField(std::string_view f) {
  static constexpr std::string_view kKnown[] = {"bn", "bt", "bu", "bv", "bver", "n", "u",
                                                "v",  "vb", "vs", "t",  "fl_"};
  for (const auto k : kKnown) {
    if (f == k) {
      return true;
    }
  }
  return false;
}

// RFC 8428 §4.5.1: name chars are A-Z a-z 0-9 - : . / _, first char alphanumeric.
bool isValidName(const std::string& name) {
  if (name.empty() || !std::isalnum(static_cast<unsigned char>(name.front()))) {
    return false;
  }
  for (const char c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != ':' && c != '.' &&
        c != '/' && c != '_') {
      return false;
    }
  }
  return true;
}

std::string at(rapidjson::SizeType i) {
  return "record " + std::to_string(i) + ": ";
}

} // namespace

tl::expected<std::vector<Record>, std::string> parse(const std::string& body, double now) {
  rapidjson::Document doc;
  if (doc.Parse(body.c_str()).HasParseError()) {
    return tl::unexpected("malformed JSON");
  }
  if (!doc.IsArray()) {
    return tl::unexpected("pack must be a JSON array");
  }

  // Base fields persist across records until redefined (RFC 8428 §4.1).
  std::string baseName;
  double baseTime = 0;
  std::string baseUnit;
  double baseValue = 0;

  std::vector<Record> records;
  records.reserve(doc.Size());

  for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
    const auto& rec = doc[i];
    if (!rec.IsObject()) {
      return tl::unexpected(at(i) + "must be an object");
    }

    for (const auto& m : rec.GetObject()) {
      const std::string_view field(m.name.GetString(), m.name.GetStringLength());
      if (!isKnownField(field) && !field.empty() && field.back() == '_') {
        return tl::unexpected(at(i) + "unknown must-understand field '" + std::string(field) +
                              "'");
      }
    }

    if (const auto it = rec.FindMember("bver"); it != rec.MemberEnd()) {
      if (!it->value.IsInt() || it->value.GetInt() > kMaxVersion) {
        return tl::unexpected(at(i) + "unsupported bver");
      }
    }
    if (const auto it = rec.FindMember("bn"); it != rec.MemberEnd()) {
      if (!it->value.IsString()) {
        return tl::unexpected(at(i) + "bn must be a string");
      }
      baseName = it->value.GetString();
    }
    if (const auto it = rec.FindMember("bt"); it != rec.MemberEnd()) {
      if (!it->value.IsNumber()) {
        return tl::unexpected(at(i) + "bt must be a number");
      }
      baseTime = it->value.GetDouble();
    }
    if (const auto it = rec.FindMember("bu"); it != rec.MemberEnd()) {
      if (!it->value.IsString()) {
        return tl::unexpected(at(i) + "bu must be a string");
      }
      baseUnit = it->value.GetString();
    }
    if (const auto it = rec.FindMember("bv"); it != rec.MemberEnd()) {
      if (!it->value.IsNumber()) {
        return tl::unexpected(at(i) + "bv must be a number");
      }
      baseValue = it->value.GetDouble();
    }

    Record out;

    std::string name = baseName;
    if (const auto it = rec.FindMember("n"); it != rec.MemberEnd()) {
      if (!it->value.IsString()) {
        return tl::unexpected(at(i) + "n must be a string");
      }
      name += it->value.GetString();
    }
    if (!isValidName(name)) {
      return tl::unexpected(at(i) + "invalid name '" + name + "'");
    }
    out.name = std::move(name);

    double time = baseTime;
    if (const auto it = rec.FindMember("t"); it != rec.MemberEnd()) {
      if (!it->value.IsNumber()) {
        return tl::unexpected(at(i) + "t must be a number");
      }
      time += it->value.GetDouble();
    }
    out.time = time < kRelativeTimeLimit ? now + time : time;

    out.unit = baseUnit;
    if (const auto it = rec.FindMember("u"); it != rec.MemberEnd()) {
      if (!it->value.IsString()) {
        return tl::unexpected(at(i) + "u must be a string");
      }
      out.unit = it->value.GetString();
    }

    int valueCount = 0;
    if (const auto it = rec.FindMember("v"); it != rec.MemberEnd()) {
      if (!it->value.IsNumber()) {
        return tl::unexpected(at(i) + "v must be a number");
      }
      out.value = baseValue + it->value.GetDouble();
      ++valueCount;
    }
    if (const auto it = rec.FindMember("vb"); it != rec.MemberEnd()) {
      if (!it->value.IsBool()) {
        return tl::unexpected(at(i) + "vb must be a boolean");
      }
      out.boolValue = it->value.GetBool();
      ++valueCount;
    }
    if (const auto it = rec.FindMember("vs"); it != rec.MemberEnd()) {
      if (!it->value.IsString()) {
        return tl::unexpected(at(i) + "vs must be a string");
      }
      out.stringValue = it->value.GetString();
      ++valueCount;
    }
    if (valueCount != 1) {
      return tl::unexpected(at(i) + "must have exactly one of v, vb, vs");
    }

    if (const auto it = rec.FindMember("fl_"); it != rec.MemberEnd()) {
      if (!it->value.IsUint() || it->value.GetUint() > UINT16_MAX) {
        return tl::unexpected(at(i) + "fl_ must be an integer in [0, 65535]");
      }
      out.flags = static_cast<uint16_t>(it->value.GetUint());
    }

    records.push_back(std::move(out));
  }

  return records;
}

} // namespace rdws::senml
