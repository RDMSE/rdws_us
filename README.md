# rdws_us

Projeto C++ modular para um sistema de análise de sensores IoT agrícolas (fazendas → campos → devices → sensores → leituras). Arquitetura de microserviços: cada recurso é um serviço independente que se conecta a um **service broker** central via socket (TCP/UNIX) e é roteado por HTTP através de um **gateway**.

## Visão geral da arquitetura

```
Cliente HTTP
    │
    ▼
HttpGateway (cpp-httplib) ── AuthMiddleware (API key / JWT HS256)
    │
    ▼
ServiceGateway (broker) ── EventRouter (resolve capability por método+path) ── EventBus (pub/sub interno)
    │  (socket TCP/UNIX, framing de mensagens)
    ▼
Microserviços (registram capabilities, processam requests, respondem)
```

- O gateway converte cada requisição HTTP num payload no contrato `LambdaEvent`/`LambdaContext` e a roteia para o microserviço capaz de atender a `capability` solicitada.
- Suporta modo síncrono (aguarda resposta com timeout configurável por capability) e assíncrono (retorna `202` + `requestId`, consultável depois via `GET /requests/{id}`).
- Observabilidade: métricas por capability (latência avg/p99, taxa de erro), `GET /health`, `GET /status`, `GET /connections`, logs estruturados (spdlog).

## Microserviços (`src/services/`)

| Serviço | Capabilities | Descrição |
|---|---|---|
| `auth_service` | `auth.login` | Autenticação; emite JWT Bearer (HS256). Endpoint público, sem auth prévia. |
| `farm_service` | `farm.list/get/create/update/delete` | CRUD de fazendas. |
| `field_service` | `field.list/get/create/update/delete` | CRUD de campos (associados a uma fazenda). |
| `device_service` | `device.list/get/create/update/delete` | CRUD de devices (associados a um campo). |
| `device_config_service` | `device_config.get/create/update/delete` | Configuração (JSONB) de cada device. |
| `sensor_service` | `sensor.list/get/create/update/delete` | CRUD de sensores (associados a um device). |
| `sensor_reading_service` | `sensor_reading.list/get` | Somente leitura (append-only) — consulta de leituras por sensor/janela de tempo. |
| `persistence_service` | — | Persistência assíncrona (bridge para eventos `request.completed`/`metrics.snapshot` publicados pelo gateway). |
| `example_service` | — | Serviço de exemplo/echo para desenvolvimento e testes manuais do broker. |

Todos seguem o mesmo padrão: `App<Nome>Service.cpp` implementa a lógica, registra as capabilities no `ServiceGateway` via `ServiceClient`, e delega persistência para `src/shared/repository/` + `src/shared/database/` (PostgreSQL via libpqxx).

## Service broker (`src/service_broker/`)

- **`ServiceGateway`** — núcleo do broker: registro/roteamento de serviços, ciclo de vida de requests (`queued → in_flight → completed/failed/timed_out`), métricas, health.
- **`HttpGateway`** — servidor HTTP (cpp-httplib) que expõe os endpoints REST e converte requisições em `LambdaEvent`.
- **`Auth/AuthMiddleware`** — autenticação por API key ou JWT (HS256), injeta identidade no `LambdaContext`.
- **`Services/EventRouter`** — roteamento dinâmico de capability por `método + path`, com condições, fallback e CRUD de regras via HTTP (`/routes`).
- **`Services/EventBus`** — pub/sub interno assíncrono (worker em background), usado para eventos internos (ex: `request.completed`, `metrics.snapshot`).
- **`Config/GatewayConfig`** — configuração por capability (timeout, estratégia de load balancing, limites), com persistência.

Ver `src/service_broker/README.md`, `SERVICE_BROKER_README.md` e `SERVICES_USAGE.md` para detalhes operacionais.

## Código compartilhado (`src/shared/`)

- **`types/`** — contrato canônico `LambdaEvent`, `LambdaContext`, `ServiceResult`.
- **`utils/json_helper.h`** — wrapper fino sobre RapidJSON: `JsonObj` (builder fluente para montar objetos), `getString/getInt/getObject/getArray/...` (leitura tipada e segura de campos), `docToString`.
- **`utils/metrics.h`** — `MetricsTracker` (latência avg/p99, taxa de erro, timeouts por capability, ring buffer de amostras).
- **`utils/response_helper.h`**, **`lambda_params_helper.h`**, **`profiler.h`**, **`logger.h`** (spdlog).
- **`validator/`** — validação de schema JSON (valijson) para payloads de request/config.
- **`database/`** — acesso PostgreSQL (libpqxx).
- **`repository/`**, **`service/`** — camadas de domínio por recurso (farm, field, device, sensor, etc.), usadas pelos microserviços correspondentes.
- **`config/`** — configuração geral (dotenv).

## Modelo de dados

PostgreSQL com PostGIS (geolocalização) e TimescaleDB (time-series para `sensor_readings`, particionado por mês). Hierarquia: `farms → fields → devices → device_configurations` / `sensors → sensor_readings`. Ver `docs/Plano_DB_IOT_Sensors.md`.

## Ingestão de dados de sensores (planejado)

Pipeline de escrita desacoplado do CRUD via HTTP: `Device --CoAP/DTLS--> IngestionService --> RabbitMQ --> ReadingWriterService --> banco`. Inclui um `SensorSimulatorService` para desenvolvimento enquanto o hardware físico não está pronto. Ver `docs/PLANO_INGESTION.md`.

## Coleção de requisições (Bruno)

O diretório `bruno/IoT Sensor API/` contém uma coleção [Bruno](https://www.usebruno.com/) com as requisições HTTP de cada endpoint (Farms, Fields, Devices, Sensors, etc.), prontas para importar e testar o gateway manualmente. Abra a pasta `bruno/IoT Sensor API/` diretamente no Bruno.

## Documentação de planejamento (`docs/`)

- `PLANO_API_REST.md` — endpoints/capabilities de cada microserviço e contrato de payload.
- `Plano_DB_IOT_Sensors.md` — modelo de dados, índices, particionamento e retenção.
- `PLANO_GATEWAY_HTTP.md` — histórico de evolução do gateway HTTP (fases concluídas e roadmap).
- `PLANO_INGESTION.md` — pipeline de ingestão de leituras (CoAP/DTLS → fila → escrita).

## Current structure

- `CMakeLists.txt`: configuração raiz e dependências (GoogleTest, cpp-httplib, spdlog, RapidJSON, tl::expected, valijson via FetchContent).
- `src/service_broker/`: gateway HTTP + broker de mensagens entre microserviços.
- `src/services/`: microserviços de domínio (ver tabela acima).
- `src/shared/`: código compartilhado (tipos, utils, validação, banco, repositórios).
- `src/greeting_app/`: subprojeto de exemplo (gerado pelo `new_subproject.sh`).
- `tools/new_subproject.sh`: gerador de novos subprojetos.

## Dependencies

| Biblioteca | Utilização | Instalação |
|---|---|---|
| `cmake` ≥ 3.20 | Build system | `sudo apt install cmake` |
| `gdb` | Debug (VS Code) | `sudo apt install gdb` |
| `libssl-dev` | Auth middleware — HMAC-SHA256 para JWT (HS256) | `sudo apt install libssl-dev` |
| `libpqxx` | Acesso PostgreSQL | via CMake config ou `pkg-config` (ver `CMakeLists.txt`) |

As restantes dependências (GoogleTest, cpp-httplib, spdlog, RapidJSON, tl::expected, valijson) são descarregadas automaticamente via CMake FetchContent na primeira build.

## Build and tests

```bash
# Instalar dependência de sistema necessária
sudo apt install libssl-dev

cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Debugging (VS Code)

Requer `gdb` instalado (`sudo apt install gdb`).

Primeiro compile em Debug Mode:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
```

The `.vscode/launch.json` has three configurations:

- **Debug service_broker_app** — inicia o broker na porta `8080` e socket `/tmp/service_broker.sock`
- **Debug example_service --dev** — inicia um service em modo dev, conecta no broker via socket
- **Debug Broker + Example Service** — sobe os dois simultaneamente (compound)

**Ordem de inicialização:** o broker deve estar no ar antes de qualquer service conectar.
Para garantir isso, inicie as configurações manualmente em sequência ou use o compound e aguarde o broker logar `ServiceBroker started successfully!` antes de interagir com o service.

## Create a new subproject

```bash
./tools/new_subproject.sh auth
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Result:

- `src/auth/CMakeLists.txt`
- `src/auth/auth.hpp`
- `src/auth/auth.cpp`
- `src/auth/main.cpp`
- `src/auth/tests/CMakeLists.txt`
- `src/auth/tests/auth_test.cpp`
