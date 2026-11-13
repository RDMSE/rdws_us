// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace rdws::utils {

// Seconds until the next refresh of caches a service loads through the gateway. While any of
// them has never loaded (at startup the owning service may not be registered yet), it retries
// soon; once all have loaded, it waits the normal cycle. A failed refresh of a cache that has
// loaded before keeps its previous content, so it can wait for the cycle.
[[nodiscard]] constexpr int nextRefreshDelaySec(bool allLoaded, int normalSec, int retrySec) {
  return allLoaded ? normalSec : retrySec;
}

} // namespace rdws::utils
