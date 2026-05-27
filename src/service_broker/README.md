# loader

loader is a project responsible for loading each service and providing a unified interface 
for the main application to interact with them.

## 🏗️ Architecture

- **biblioteca interna `loader_core`**: Core functionality with JSON schema validation
- **executavel `loader_app`**: Standalone application demonstrating service loading
- **testes unitarios em `tests/`**: Comprehensive test suite (20 tests ✅)

### New Features ✨
- **JSON Schema Validation**: Uses `SERVICES_ARRAY_SCHEMA` to validate configuration files
- **Safe Loading**: Validates before loading, prevents invalid configurations  
- **Rich API**: Search, iterate, and access services safely
- **Comprehensive Error Handling**: Detailed error messages for debugging

## 📁 Components

### Core Classes
- **`loader::services`**: Main service loader with validation ([SERVICES_USAGE.md](SERVICES_USAGE.md))
- **`loader::service`**: Individual service representation
- **`loader::schemas::SERVICE_SCHEMA`**: JSON Schema for validation

### Configuration Files
- **`services.json`**: Service definitions (validated automatically)
- **`loader_services.json`**: Alternative configuration format

## 🚀 Quick Start

```cpp
#include "services.h"

// Load and validate services
loader::services serviceLoader("services.json");

// Access loaded services
for (const auto& service : serviceLoader) {
    std::cout << "Loading: " << service.getName() 
              << " (" << service.getInstances() << " instances)" << std::endl;
}

// Find specific service
if (auto* svc = serviceLoader.findServiceByName("my-service")) {
    // Launch service...
}
```

## Build and tests

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Test Results
- **Schema Validation**: 5 tests ✅ 
- **Service Loading**: 7 tests ✅
- **Loader Core**: 2 tests ✅  
- **Shared Components**: 4 tests ✅

**Total: 20/20 tests passing (100%)** 🎉

## Dependencies

- **Shared Validation**: `rdws_shared_validation` library
- **ValiJSON**: JSON Schema validation
- **JsonCpp**: JSON parsing and manipulation  
- **inih**: INI file support (planned)

See [schemas/README.md](schemas/README.md) for schema documentation.

