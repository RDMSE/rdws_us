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
| `device_service` | `device.list/get/create/update/delete` + `device_credential.get_active/rotate/revoke/list_active` (internas) | CRUD de devices (associados a um campo) + credenciais PSK por device (`device_credentials`, AES-256-GCM em repouso). As `device_credential.*` são internas — ausentes de `routes.json`, só chamáveis via `ServiceClient::invoke` por outro serviço. |
| `sensor_service` | `sensor.list/get/create/update/delete` | CRUD de sensores (associados a um device). |
| `sensor_reading_service` | `sensor_reading.list/get` | Somente leitura (append-only) — consulta de leituras por sensor/janela de tempo. |
| `persistence_service` | — | Persistência assíncrona (bridge para eventos `request.completed`/`metrics.snapshot` publicados pelo gateway). |
| `ingestion_service` | — (cliente puro) | Servidor CoAP/DTLS (libcoap, PSK) que recebe leituras de devices/`SensorSimulatorService`, valida formato mínimo e publica no RabbitMQ (`sensor_readings`, uma mensagem por leitura). Stateless quanto à persistência — sem conexão direta ao Postgres. |
| `reading_writer_service` | — (worker puro) | Consome a fila `sensor_readings` e grava em `sensor_readings` (Postgres), idempotente via `UNIQUE(sensor_id, timestamp)`. Sem `ServiceClient`/capability no gateway — não expõe nada, só processa a fila. |
| `example_service` | — | Serviço de exemplo/echo para desenvolvimento e testes manuais do broker. |

Todos seguem o mesmo padrão: `App<Nome>Service.cpp` implementa a lógica, registra as capabilities no `ServiceGateway` via `ServiceClient`, e delega persistência para `src/shared/repository/` + `src/shared/database/` (PostgreSQL via libpqxx). Exceções: `ingestion_service` (`ServiceClient` só como cliente, sem capabilities próprias) e `reading_writer_service` (nem se conecta ao gateway).

Fora dessa tabela, `SensorSimulatorService` (`src/services/sensor_simulator_service/`) é uma
ferramenta **standalone**, deliberadamente fora do compose principal (ver
`docs/Plano_SensorSimulatorService.md`) — gera leituras simuladas e as envia por
CoAP/DTLS pro `ingestion_service`, útil pra testar o pipeline sem hardware real.

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

## Ingestão de dados de sensores

Pipeline de escrita desacoplado do CRUD via HTTP, implementado e validado ponta a ponta:
`Device/SensorSimulatorService --CoAP/DTLS--> IngestionService --> RabbitMQ -->
ReadingWriterService --> banco`. Autenticação por device via PSK
(`device_credentials`, AES-256-GCM em repouso). Inclui um `SensorSimulatorService`
standalone para desenvolvimento enquanto o hardware físico não está pronto (gera
leituras simuladas, dispara via `POST /simulate/{device_id}/trigger` — ver coleção
Bruno). Ver `docs/Plano_Ingestion.md`, `docs/Plano_Ingestion_Implementacao.md` (detalhamento
+ runbook de como subir tudo localmente) e `docs/Plano_SensorSimulatorService.md`.

**Pendente**: validação completa de payload contra `device_config` no `IngestionService`
(hoje só formato mínimo) e invalidação de credencial em tempo real (hoje poll de 60s em
vez de invalidação via evento).

## Coleção de requisições (Bruno)

O diretório `bruno/IoT Sensor API/` contém uma coleção [Bruno](https://www.usebruno.com/) com as requisições HTTP de cada endpoint (Farms, Fields, Devices, Sensors, etc.), prontas para importar e testar o gateway manualmente. Abra a pasta `bruno/IoT Sensor API/` diretamente no Bruno.

## Observabilidade (Prometheus + Loki + Grafana)

O gateway expõe métricas em `GET /metrics/prometheus` (formato texto do Prometheus) além
do `GET /metrics` em JSON. Logs estruturados JSON já são escritos por padrão (`stdout` +
arquivo rotativo `logs/rdws-gateway.log`, sem configuração extra). Duas pilhas de
observabilidade, uma por ambiente — nunca dividem dados entre si e têm ciclo de vida
diferente:

| | QA (homelab) | dev (sua máquina) |
|---|---|---|
| Arquivo | `docker-compose.qa-observability.yml` | `docker-compose.dev-observability.yml` |
| Ciclo de vida | sempre no ar (`restart: unless-stopped`) | sob demanda — sem `restart:`, sobe só quando precisar |
| Prometheus escrapa | `gateway:3001` via DNS interno do Compose | `host.docker.internal:3001` (gateway roda nativo no host) |
| Promtail coleta logs | `stdout` dos containers via Docker service discovery | arquivo `logs/rdws-gateway.log` montado read-only |
| Portas | Prometheus 9090, Loki 3100, Grafana 3300 | Prometheus 9091, Loki 3101, Grafana 3301 |

Datasources e um dashboard inicial (`RDWS Gateway — Overview`) já vêm provisionados via
arquivo (`infra/grafana/provisioning/`) — nada pra configurar manualmente na UI do
Grafana.

### Usando em dev

1. Rode o gateway localmente como sempre (task `run-gateway` do VS Code, ou direto):
   ```bash
   ./build/src/service_broker/service_gateway_http 8080 3001 /tmp/service_gateway.sock
   ```
2. Suba a pilha de observabilidade só quando for investigar algo:
   ```bash
   docker compose -f docker-compose.dev-observability.yml up -d
   ```
3. Acesse o Grafana em [http://localhost:3301](http://localhost:3301) (`admin`/`admin` no
   primeiro acesso — ou defina `GRAFANA_ADMIN_PASSWORD` antes do `up`). O dashboard
   `RDWS Gateway — Overview` já mostra métricas (Prometheus) e logs (Loki) do gateway
   local. Prometheus cru em [http://localhost:9091](http://localhost:9091).
4. Quando terminar, derrube pra não deixar consumindo recursos à toa:
   ```bash
   docker compose -f docker-compose.dev-observability.yml down
   ```

### Usando em QA

**Não sobe sozinha** — nem no primeiro deploy nem nos seguintes: o workflow
`deploy-qa.yml` só sobe `docker-compose.qa-db.yml` (Postgres) e `docker-compose.qa-app.yml`
(gateway + serviços), nunca o de observabilidade. É um passo manual, uma vez só
(depois fica no ar com `restart: unless-stopped`):

```bash
docker compose -f docker-compose.qa-observability.yml --env-file .env.qa up -d
```

- Precisa do `docker-compose.qa-app.yml` já rodando antes (Prometheus escrapa o gateway
  pela rede do Compose).
- Mesmo `.env.qa` que os outros compose de QA já usam — nenhuma variável nova.
- Acesse via Tailscale: Grafana em `http://fedora-server:3300`, Prometheus em
  `http://fedora-server:9091`.
- Só precisa repetir o `up -d` se derrubou manualmente ou mudou algo no compose — não a
  cada deploy do app.

**Dica pra quem está começando com Docker**: `up -d` sobe em background; `docker compose
-f <arquivo> ps` mostra se os containers subiram e o status; `docker compose -f
<arquivo> logs -f <serviço>` (ex. `grafana`) mostra o log ao vivo de um container
específico se algo não aparecer no navegador.

Ver `docs/Plano_Deployment.md` §3 para os detalhes de infraestrutura e a decisão de
design por trás da separação dev/QA.

## Documentação de planejamento (`docs/`)

- `Plano_API_REST.md` — endpoints/capabilities de cada microserviço e contrato de payload.
- `Plano_DB_IOT_Sensors.md` — modelo de dados, índices, particionamento e retenção.
- `Plano_Gateway_HTTP.md` — histórico de evolução do gateway HTTP (fases concluídas e roadmap).
- `Plano_Ingestion.md` — pipeline de ingestão de leituras (CoAP/DTLS → fila → escrita).
- `Plano_Ingestion_Implementacao.md` — detalhamento de implementação (IngestionService, RabbitMQ, ReadingWriterService) + runbook de como subir tudo localmente e disparar via Bruno.
- `Plano_DeviceCredentials.md` — provisionamento/rotação de credenciais PSK por device.
- `Plano_Alerting.md` — detecção de condições anormais (limites, dewpoint) e geração de alarmes por fazenda.
- `Plano_Deployment.md` — dockerização e CI/CD para os ambientes dev local, QA homelab e prod VPS.
- `Plano_SensorSimulatorService.md` — plano da ferramenta `Sensor Simulator Service`, usada para simular dados de sensores enquanto o hardware físico não está disponível.
- `Plano_SensorSimulatorService_Implementacao.md` — detalhamento de implementação (device_credentials, is_simulated, cliente CoAP/DTLS).

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
| `libssl-dev` | HMAC-SHA256 (JWT), AES-256-GCM (`device_credentials` em repouso) e backend DTLS/TLS do `libcoap`/`rabbitmq-c` | `sudo apt install libssl-dev` |
| `libpqxx` | Acesso PostgreSQL | via CMake config ou `pkg-config` (ver `CMakeLists.txt`) |

As restantes dependências (GoogleTest, cpp-httplib, spdlog, RapidJSON, tl::expected, valijson, `libcoap`, `rabbitmq-c`) são descarregadas e compiladas automaticamente via CMake FetchContent na primeira build — estático, sem pacote `apt` adicional além do `libssl-dev` acima.

**Dependência de runtime (não de build)**: `ingestion_service`/`reading_writer_service` precisam de um broker RabbitMQ acessível (`RABBITMQ_HOST`/`PORT`/`USER`/`PASSWORD`) para funcionar — não é baixado pelo CMake, é um serviço externo (ver `docker-compose.qa-mq.yml` ou suba um container avulso pra testar localmente, `docs/Plano_Ingestion_Implementacao.md` tem o comando exato).

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
