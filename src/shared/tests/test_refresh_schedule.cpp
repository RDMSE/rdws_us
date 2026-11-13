// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "utils/refresh_schedule.h"

#include <gtest/gtest.h>

using rdws::utils::nextRefreshDelaySec;

TEST(RefreshSchedule, RetriesSoonWhileACacheHasNeverLoaded) {
  EXPECT_EQ(nextRefreshDelaySec(/*allLoaded=*/false, 60, 5), 5);
}

TEST(RefreshSchedule, NormalCycleOnceEveryCacheLoaded) {
  EXPECT_EQ(nextRefreshDelaySec(/*allLoaded=*/true, 60, 5), 60);
}

// Compile-time too: the ingestion uses it in its refresh loop
static_assert(nextRefreshDelaySec(false, 60, 5) == 5);
static_assert(nextRefreshDelaySec(true, 60, 5) == 60);
