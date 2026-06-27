#pragma once

#include <filesystem>

namespace rdws::loader::utils {
[[nodiscard]] std::filesystem::path resolveServiceConfigFilePath(int argc, char* argv[]);
};
