#include "Utils.h"

std::filesystem::path rdws::loader::utils::resolveServiceConfigFilePath(int argc, char* argv[]) {
  if (argc > 1) {
    // Use path from command line argument if provided
    return argv[1];
    }

    // Try to find services.json relative to executable location
    const std::filesystem::path execPath = std::filesystem::canonical(argv[0]);
    const std::filesystem::path execDir = execPath.parent_path();

    // Look for services.json in same directory as executable
    std::filesystem::path servicesFile = execDir / "services.json";

    // If not found, try current directory
    if (!std::filesystem::exists(servicesFile)) {
        servicesFile = "./services.json";
    }

    // If still not found, try going up to find src/loader/services.json
    if (!std::filesystem::exists(servicesFile)) {
        const auto currentDir = std::filesystem::current_path();
        servicesFile = currentDir / "src" / "loader" / "services.json";
    }
    return servicesFile;
}
