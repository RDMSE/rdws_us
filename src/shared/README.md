# Shared Components

Bibliotecas usadas por mais de um serviço do `rdws_us`. Licença Apache-2.0
([`LICENSE`](LICENSE)), para que serviços de terceiros possam usá-las e falar com o gateway
sem herdar a AGPL do resto do projeto (ver "Licença" no README da raiz).

## Estrutura

| Pasta | Conteúdo | Target CMake |
|---|---|---|
| `types/` | Contrato canônico `LambdaEvent`, `LambdaContext`, `ServiceResult` | `rdws_shared_types` |
| `utils/` | `json_helper` (wrapper do RapidJSON: `JsonObj`, `getString/getInt/...`, `docToString`), `json_merge` (merge patch, RFC 7396), `response_helper`, `lambda_params_helper`, `metrics` (latência avg/p99, taxa de erro), `profiler`, `capability_router` | `rdws_shared_json_utils`, `rdws_shared_utils` |
| `utils/logger.*` | Logger estruturado (spdlog), JSON em `stdout` + arquivo rotativo | `rdws_shared_logger` |
| `config/` | Configuração por variáveis de ambiente / dotenv (`RDWS_ENVIRONMENT`, ...) | `rdws_shared_config` |
| `validator/` | `SchemaValidator`: JSON Schema draft-07 (valijson) sobre `rapidjson::Document` | `rdws_shared_validation` |
| `database/` | `IDatabase` + `PostgreSQLDatabase` (libpqxx), com `DatabaseError::sqlstate()` | `rdws_shared_database` |
| `repository/` | Acesso a dados por recurso (farm, field, device, sensor, leituras, credenciais, config, telemetria, atividade do device) | — |
| `service/` | Lógica de domínio usada pelos `App*Service` (inclui `DeviceConfigService` com o JSON Schema do `weather_station` e as checagens de `onboard[]`/`triggers[]`) | — |
| `crypto/` | `credential_cipher`: AES-256-GCM das PSKs em repouso (`device_credentials`) | `rdws_shared_crypto` |
| `coap/` | `CoapDtlsClient`: cliente CoAP/DTLS-PSK (libcoap) usado pelo `SensorSimulatorService` | `rdws_shared_coap` |
| `senml/` | Parser SenML JSON (RFC 8428), conversão de unidades SenML ↔ `sensors.unit` e `configForFirmware` (ponto fixo dos `triggers[]`) | `rdws_shared_senml` |
| `amqp/` | `AmqpProducer`/`AmqpConsumer` (rabbitmq-c), com publisher confirms e `publishBatch` | `rdws_shared_amqp` |
| `tests/` | Testes GoogleTest de cada componente | `shared_*_tests` |

Contratos que estas bibliotecas implementam: `docs/Plano_Telemetria.md` (SenML, unidades),
`docs/Plano_DeviceCredentials.md` (cifra das PSKs) e `docs/Plano_Ingestion.md` (fila).

## Uso

```cmake
target_link_libraries(seu_target PRIVATE rdws_shared_validation rdws_shared_json_utils)
target_include_directories(seu_target PRIVATE ${CMAKE_SOURCE_DIR}/src/shared)
```

Exemplo, `SchemaValidator`:

```cpp
#include "validator/schema_validator.h"

auto validator = rdws::utils::validator::SchemaValidator::fromString("my_schema", schemaJson);

rapidjson::Document data;
data.Parse(body.c_str());
if (!validator.isValid(data)) {
    for (const auto& error : validator.validate(data)) {
        std::cout << error.field << ": " << error.message << '\n';
    }
}
```

## Testes

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build -R shared_ --output-on-failure
```

## Convenções

- Namespace `rdws::*` (ex.: `rdws::utils::validator`, `rdws::senml`).
- Headers `.h`, implementações `.cpp`, testes `tests/test_<componente>.cpp`.
- Cabeçalho SPDX `Apache-2.0` em todo arquivo-fonte desta pasta (o CI confere,
  `scripts/check_spdx.sh`).
- Funções puras quando possível (o parser SenML e o `configForFirmware` não fazem I/O), para
  testar sem banco nem rede.
