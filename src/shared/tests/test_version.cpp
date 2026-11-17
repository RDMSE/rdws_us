// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "rdws_version.h"

#include <gtest/gtest.h>

#include <string>

using rdws::kVersion;
using rdws::versionFrom;

TEST(Version, FromTheEnvironmentWhenSet) {
  // The Docker images: RDWS_VERSION set in the runtime stage, the compiled one is a placeholder
  EXPECT_STREQ(versionFrom("v0.3.0-2-g1a2b3c4"), "v0.3.0-2-g1a2b3c4");
}

TEST(Version, CompiledOneWhenUnsetOrEmpty) {
  // Local builds: no RDWS_VERSION, the `git describe` they were configured with
  EXPECT_STREQ(versionFrom(nullptr), kVersion);
  EXPECT_STREQ(versionFrom(""), kVersion);
}

TEST(Version, CompiledOneLooksLikeAVersion) {
  EXPECT_EQ(std::string(kVersion).rfind('v', 0), 0u) << kVersion;
}
