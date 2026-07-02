# Service Schema Usage Example

Este arquivo demonstra como usar os schemas de validação criados em `schemas/service.h` para validar arquivos JSON de serviços.

## ✅ Schemas Disponíveis

### 1. `SERVICE_SCHEMA`
Valida um único objeto de serviço:
```json
{
    "name": "service-test1",
    "path": "./services/test1",
    "instances": 1
}
```

### 2. `SERVICES_ARRAY_SCHEMA`  
Valida arrays de serviços (como `services.json`):
```json
[
    {
        "name": "service-test1",
        "path": "./services/test1",
        "instances": 1
    },
    {
        "name": "service-test2", 
        "path": "./services/test2",
        "instances": 2
    }
]
```

## 💻 Como Usar no Código

```cpp
#include "validator/schema_validator.h"
#include "schemas/service.h"

using namespace rdws::utils::validator;
using namespace loader::schemas;

// Validar um arquivo services.json
bool validateServicesFile(const std::string& filePath) {
    auto validator = SchemaValidator::fromString("services", SERVICES_ARRAY_SCHEMA);
    
    // Ler arquivo JSON
    std::ifstream file(filePath);
    Json::Value servicesJson;
    file >> servicesJson;
    
    // Validar
    auto errors = validator.validate(servicesJson);
    
    if (errors.empty()) {
        std::cout << "✅ Services file is valid!" << std::endl;
        return true;
    } else {
        std::cout << "❌ Validation errors found:" << std::endl;
        for (const auto& error : errors) {
            std::cout << "  - Field: " << error.field 
                      << ", Error: " << error.message << std::endl;
        }
        return false;
    }
}
```

## 🔄 Integração com a Classe `services`

Os schemas podem ser usados na classe `services` para validar dados antes de criar objetos `service`:

```cpp
// Em services.cpp
bool services::loadFromJson(const std::filesystem::path& jsonFilePath) {
    // 1. Validar o JSON com schema
    auto validator = SchemaValidator::fromString("services", SERVICES_ARRAY_SCHEMA);
    
    if (!validator.validateFile(jsonFilePath)) {
        return false;
    }
    
    // 2. Se válido, carregar os serviços
    // ... resto da implementação
}
```

## 🧪 Testes Incluídos

Os seguintes testes garantem que os schemas funcionam corretamente:

- ✅ `ValidatesSingleServiceObject` - Testa validação de objeto individual
- ✅ `RejectsServiceWithMissingName` - Rejeita quando falta campo obrigatório  
- ✅ `RejectsServiceWithInvalidInstancesCount` - Rejeita valores negativos
- ✅ `ValidatesServicesArray` - Valida arrays de serviços
- ✅ `ValidatesExistingServicesJsonStructure` - Valida formato atual do projeto

Todos os testes estão passando (13/13) ✅