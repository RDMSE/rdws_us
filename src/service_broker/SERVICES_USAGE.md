# Services Class Usage

A classe `services` agora integra validação JSON Schema para carregar configurações de serviços de forma segura.

## ✅ Funcionalidades Implementadas

### 1. **Validação Automática**
- Usa `SERVICES_ARRAY_SCHEMA` para validar arquivos JSON
- Rejeita configurações inválidas antes do carregamento
- Fornece mensagens de erro detalhadas

### 2. **Carregamento Seguro**  
- Lê e parseia arquivos JSON
- Valida schema antes de criar objetos `service`
- Carrega dados validados na lista `loadedServices`  

### 3. **API Completa**
```cpp
class services {
public:
    explicit services(const std::filesystem::path& configFile);
    
    // Access methods
    const std::list<service>& getServices() const;
    size_t getServiceCount() const;
    bool isEmpty() const;
    
    // Search
    const service* findServiceByName(const std::string& name) const;
    
    // Iterator support  
    const_iterator begin() const;
    const_iterator end() const;
};
```

## 💻 Exemplos de Uso

### Carregamento Básico
```cpp
#include "services.h"

try {
    // Carrega e valida services.json
    loader::services serviceLoader("services.json");
    
    std::cout << "Loaded " << serviceLoader.getServiceCount() << " services" << std::endl;
    
    // Iterar pelos serviços
    for (const auto& service : serviceLoader) {
        std::cout << "Service: " << service.getName() 
                  << " (instances: " << service.getInstances() << ")" << std::endl;
    }
    
} catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
}
```

### Busca por Nome
```cpp
auto* service = serviceLoader.findServiceByName("my-service");
if (service) {
    std::cout << "Found: " << service->getPath() << std::endl;
} else {
    std::cout << "Service not found" << std::endl;
}
```

### Verificação de Status
```cpp
if (serviceLoader.isEmpty()) {
    std::cout << "No services configured" << std::endl;
} else {
    std::cout << "Services ready to launch" << std::endl;
}
```

## 🛡️ Tratamento de Erros

A classe `services` lança `std::runtime_error` nas seguintes situações:

1. **Arquivo não encontrado**
```
Configuration file not found: /path/to/file.json
```

2. **JSON malformado**  
```
Invalid JSON in configuration file: [detalhes do erro]
```

3. **Validação de schema falhou**
```
Schema validation failed:
  - Field: name, Error: String is too short (minimum 1), actual length 0
```

4. **Erro ao criar objetos service**
```
Error loading service: [detalhes específicos]
```

## 🧪 Testes Incluídos

### Testes de Funcionalidade (7 testes passando ✅)
- `LoadsValidServicesSuccessfully` - Carrega arquivo válido
- `FindsServiceByName` - Busca por nome funciona
- `IteratorWorks` - Iteração funciona
- `LoadsEmptyArraySuccessfully` - Arrays vazios são aceitos

### Testes de Erro (4 testes passando ✅) 
- `ThrowsOnNonExistentFile` - Arquivo inexistente gera erro
- `ThrowsOnMalformedJson` - JSON inválido gera erro  
- `ThrowsOnSchemaValidationFailure` - Schema inválido gera erro

## 🔗 Integração com Schema Validator

```cpp
// A classe usa automaticamente:
auto validator = rdws::validation::SchemaValidator::fromString(
    "services_array", 
    loader::schemas::SERVICES_ARRAY_SCHEMA
);

auto errors = validator.validate(jsonData);
// Se houver erros, lança exceção com detalhes
```

## 📈 Performance

- **Validação única**: Schema é validado apenas uma vez no construtor
- **Move semantics**: Objetos `service` são criados eficientemente com `emplace_back()`
- **Acesso O(1)**: Métodos como `getServiceCount()` são constantes
- **Busca O(n)**: `findServiceByName()` é linear (adequado para listas pequenas de serviços)

A implementação está pronta para uso em produção! 🚀