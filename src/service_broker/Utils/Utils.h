// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <filesystem>

namespace rdws::loader::utils {
[[nodiscard]] std::filesystem::path resolveServiceConfigFilePath(int argc, char* argv[]);
};
