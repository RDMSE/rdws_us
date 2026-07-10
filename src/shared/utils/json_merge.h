#pragma once
#include <string>

namespace rdws::utils::json {

/// Merge Patch (RFC 7396-like, one level — not recursive in nested objects):
/// patch keys missing on `base` are not touched; keys present overwrite
/// the value in `base`; key with value `null` removes the key from `base`.
///
/// invalid/non-object `base` is treated as `{}`. invalid/non-object `patch` returns
/// `base` unchanged (no changes applied).
[[nodiscard]] std::string mergePatch(const std::string& base, const std::string& patch);

} // namespace rdws::utils::json
