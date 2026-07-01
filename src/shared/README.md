# Shared Components

Este diretório contém componentes compartilhados que podem ser utilizados por múltiplos subprojetos do rdws_us.

## 📁 Estrutura

```
src/shared/
├── CMakeLists.txt           # Build configuration
├── README.md               # Esta documentação
├── validator/              # Componentes de validação
│   ├── schema_validator.h  # Header da classe SchemaValidator
│   └── schema_validator.cpp # Implementação
└── tests/                  # Testes unitários
    ├── CMakeLists.txt      # Build configuration dos testes
    └── test_schema_validator.cpp # Testes do SchemaValidator
```

## 🔧 Componentes Disponíveis

### SchemaValidator

Classe para validação de JSON usando schemas JSON Schema Draft 7.

**Funcionalidades:**
- Validação de objetos JSON contra schemas
- Suporte a strings JSON e objetos Json::Value
- Relatórios detalhados de erros de validação
- Factory method seguro para criação de instâncias

**Exemplo de uso:**
```cpp
#include "validator/schema_validator.h"

// Criar validator a partir de schema string
auto validator = rdws::utils::validator::SchemaValidator::fromString("my_schema", schemaJson);

// Validar JSON
Json::Value data = /* seu JSON */;
if (validator.isValid(data)) {
    // JSON válido
} else {
    auto errors = validator.validate(data);
    for (const auto& error : errors) {
        std::cout << "Erro no campo '" << error.field 
                  << "': " << error.message << std::endl;
    }
}
```

## 🔗 Dependências

- **ValiJSON**: Biblioteca de validação JSON (via third_party)
- **JsonCpp**: Para manipulação de objetos JSON
- **GoogleTest**: Para testes unitários (opcional)

## 🏗️ Como Usar

### 1. Em outros projetos CMake

```cmake
target_link_libraries(seu_target PRIVATE rdws_shared_validation)
target_include_directories(seu_target PRIVATE ${CMAKE_SOURCE_DIR}/src/shared)
```

### 2. Build e Testes

```bash
# Build completo
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build

# Executar testes dos componentes shared
ctest --test-dir build -R shared_validation_tests
```

## 📋 Casos de Uso

1. **Loader**: Validar arquivos `services.json` e `loader_services.json`
2. **Greeting App**: Validar configurações de entrada
3. **Futuros projetos**: Qualquer validação de JSON necessária

## 🚀 Adicionando Novos Componentes

1. Criar header e implementação na pasta apropriada
2. Atualizar `CMakeLists.txt` com novos arquivos
3. Adicionar testes em `tests/`
4. Documentar no README

## 💡 Convenções

- Namespace: `rdws::*` (e.g., `rdws::utils::validator`)
- Headers com `.h`, implementações com `.cpp`
- Testes prefixados com `test_`
- Use factory methods para construção complexa
- Prefira RAII e smart pointers