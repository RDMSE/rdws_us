// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <rapidjson/document.h>

namespace rdws::utils {

// For sensitive capabilities routed over HTTP: only role=admin may call them. Requests without an
// injected identity (auth mode NONE in dev, or an internal ServiceClient::invoke) are let through,
// same as every other route. The error response, or nullopt to go on.
[[nodiscard]] std::optional<rapidjson::Document> requireAdmin(const rapidjson::Document& request);

// For service-only capabilities: the gateway injects an identity into every authenticated HTTP
// request, and a service's ServiceClient::invoke carries none. So any identity here means the
// request came from an HTTP client.
[[nodiscard]] std::optional<rapidjson::Document> requireInternal(const rapidjson::Document& request);

} // namespace rdws::utils
