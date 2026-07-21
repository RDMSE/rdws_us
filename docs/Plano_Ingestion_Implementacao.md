Data: 2026-07-16

# Detalhamento de implementação — `IngestionService` + RabbitMQ + `ReadingWriterService`

Este documento detalha a implementação da última etapa do pipeline de ingestão
(`Plano_Ingestion.md`): o `IngestionService` (servidor CoAP/DTLS), a fila RabbitMQ, e o
`ReadingWriterService` (consumidor → Postgres).

## Contexto

Com o `SensorSimulatorService` validado ponta a ponta (envia CoAP/DTLS para um servidor
de teste ad-hoc), falta a peça real do lado servidor: `IngestionService` (recebe e
valida), uma fila persistente (`ReadingWriterService` consome), e a escrita em
`sensor_readings`. `Plano_Ingestion.md` já define o desenho de alto nível; este plano
cobre os três componentes juntos, pela mesma razão que o próprio `Plano_Ingestion.md` já
registra: só com os três dá pra validar o pipeline ponta a ponta de verdade (simulador →
IngestionService → RabbitMQ → ReadingWriterService → banco).

Pesquisa confirmou 3 lacunas reais que este plano precisa fechar antes de implementar
(nenhuma delas resolvida ainda no código):
1. Não existe capability de bulk-fetch de credenciais (`device_credential.get_active` só
   busca uma por vez) — `IngestionService` precisa carregar todas as credenciais ativas
   no startup.
2. `EventBus` é interno ao processo do gateway — um serviço backend (`IngestionService`)
   não consegue `subscribe()` nele diretamente. `device_credential.changed` (invalidação
   de cache em tempo real) exigiria um bridge novo no lado do gateway, no mesmo padrão
   do `persistence_service` (`ServiceGateway::start()`). Fora de escopo por ora — ver
   simplificação abaixo.
3. `sensor_readings` não tem `UNIQUE(sensor_id, timestamp)` — só índice, não constraint —
   e `SensorReadingRepository` é somente-leitura hoje (sem `insert`).

**Simplificação deliberada (documentada, não é lacuna):** em vez do bridge de EventBus
para `device_credential.changed`, o `IngestionService` recarrega as credenciais ativas
periodicamente (poll, ex. a cada 60s) via `device_credential.list_active` — mesmo
princípio que o `SensorSimulatorService` já usa (buscar de novo em vez de manter
cache+invalidação), só que em lote e periódico em vez de por ciclo. Troca invalidação
instantânea por simplicidade; revogação leva até 60s pra propagar, aceitável pra este
estágio. Registrar isso como ponto em aberto no `Plano_DeviceCredentials.md` quando a
implementação for concluída.

## Parte 1 — Bulk credential lookup

- `DeviceCredentialRepository::findAllActive()` — `SELECT ... WHERE status='active'`,
  reaproveitando `credentialFromRow` (`src/shared/repository/DeviceCredentialRepository.cpp`).
- Nova capability interna `device_credential.list_active` em `AppDeviceService.cpp`,
  mesmo padrão das outras 3 (`get_active`/`rotate`/`revoke`) — **ausente de `routes.json`**.
  Handler retorna array de `{psk_identity, psk_key (hex)}`.

## Parte 2 — `sensor_readings`: idempotência + escrita

- Migration `V8__sensor_readings_unique.sql`: `ALTER TABLE sensor_readings ADD CONSTRAINT
  sensor_readings_sensor_ts_key UNIQUE (sensor_id, timestamp)`.
- `SensorReadingRepository` (`src/shared/repository/SensorReadingRepository.h/.cpp`,
  hoje só leitura) ganha `insert(sensorId, timestamp, value) -> bool`, usando
  `INSERT INTO sensor_readings (sensor_id, timestamp, value) VALUES ($1,$2,$3) ON
  CONFLICT (sensor_id, timestamp) DO NOTHING` via `db_.execCommand` (mesmo padrão de
  parâmetros posicionais já usado em `DeviceRepository`/`DeviceCredentialRepository`).

## Parte 3 — RabbitMQ (dependência nova)

**Biblioteca**: `rabbitmq-c` (C, AMQP 0-9-1) via FetchContent em
`src/third_party/CMakeLists.txt`, mesmo padrão `URL` + timeout já usado pra
libcoap/fmt/spdlog/httplib:
```cmake
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_API_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_SSL_SUPPORT ON CACHE BOOL "" FORCE)  # já temos OpenSSL como dependência
FetchContent_Declare(rabbitmq_c
    URL https://github.com/alanxz/rabbitmq-c/archive/refs/tags/v0.15.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE TIMEOUT 60 INACTIVITY_TIMEOUT 30)
FetchContent_MakeAvailable(rabbitmq_c)
```
Build estático — sem pacote apt novo no `Dockerfile` (mesma vantagem que libcoap: builder
stage já tem `libssl-dev`/`cmake`/`build-essential`; runtime não muda).

**Wrapper fino** `src/shared/amqp/amqp_client.h/.cpp` (mesmo espírito do
`CoapDtlsClient`): `AmqpProducer` (connect, declare queue durável, `publish(routingKey,
body)`) e `AmqpConsumer` (connect, declare queue, `consumeBlocking(handler, timeoutMs)`
chamando `handler` por mensagem, ack manual após handler ter sucesso). API exata
(`amqp_new_connection`, `amqp_tcp_socket_new`, `amqp_login`, `amqp_channel_open`,
`amqp_queue_declare`, `amqp_basic_publish`, `amqp_basic_consume`,
`amqp_consume_message`, `amqp_basic_ack`) — confirmar assinaturas exatas contra a versão
vendorizada no momento da implementação.

**Fila**: uma queue durável `sensor_readings` — mensagem por **leitura individual**
(`device_id`, `sensor_id`, `timestamp`, `valor`, `unidade`), conforme
`Plano_Ingestion.md` já especifica — não por lote, simplifica o `ReadingWriterService`
(uma mensagem = uma linha).

**Infra**: `docker-compose.qa-mq.yml` novo (mesmo padrão de split
`qa-db`/`qa-app`, `name: rdws_qa` compartilhado) — imagem oficial
`rabbitmq:3-management`, plugin Prometheus habilitado (`rabbitmq_prometheus`), sem build
próprio. Adiado para quando estes dois serviços realmente existirem (já é o caso agora),
conforme `Plano_Deployment.md` §6 passo 5.

## Parte 4 — `IngestionService`

Novo `src/services/ingestion_service/`, mesmo padrão de scaffolding dos demais serviços
(`ServiceIdentity`, `ServiceClient`, `main()` com `<serviceId> <machineName>
<gatewayAddress>`), registrado em `src/services/CMakeLists.txt`.

- **Stateless quanto à persistência** (não conecta no Postgres) — só ao gateway (para
  `device_credential.list_active`) e ao RabbitMQ (produtor).
- **Servidor CoAP/DTLS** via `libcoap` (`coap_context_set_psk2` com
  `validate_id_call_back` consultando o cache em memória `psk_identity → chave`,
  `coap_new_endpoint` com `COAP_PROTO_DTLS`, resource handler em POST na raiz — mesmo
  padrão do servidor de teste usado na verificação manual do `SensorSimulatorService`).
- **Thread de refresh do cache**: chama `device_credential.list_active` a cada 60s
  (startup síncrono + loop periódico) — ver simplificação acima.
- **Handler de request**: parseia o payload (mesmo formato que `SensorSimulatorService`
  já envia — `{"device_id":...,"readings":[{sensor_id,timestamp,value,unit}]}`), faz só
  **validação mínima de formato** (campos obrigatórios presentes, tipos corretos) —
  **decisão explícita**: validação completa contra `device_config` (schema/faixas
  plausíveis) fica para uma iteração futura, registrada como pendência no
  `Plano_Ingestion.md` ao final desta implementação (`IngestionService` continua
  stateless quanto à persistência, sem conexão direta ao Postgres nesta entrega — a
  forma de acessar `device_config` de um serviço stateless, provavelmente uma nova
  capability, fica em aberto). Publica **uma mensagem por leitura** no RabbitMQ, responde
  ACK CoAP.

## Parte 5 — `ReadingWriterService`

Novo `src/services/reading_writer_service/` — **worker puro, sem `ServiceClient`/
capability no gateway** (conforme `Plano_Ingestion.md`: "não expõe endpoints HTTP").
- Conexão direta ao Postgres (`PostgreSQLDatabase`, mesmo padrão dos demais).
- `AmqpConsumer` consumindo a fila `sensor_readings` em loop bloqueante; por mensagem,
  chama `SensorReadingRepository::insert(...)` (idempotente via `ON CONFLICT DO
  NOTHING`, Parte 2) e só faz `amqp_basic_ack` depois do insert confirmado — mensagem
  não é perdida se o processo cair no meio.

## Arquivos de referência (reaproveitar padrão)
- `src/shared/coap/coap_dtls_client.h/.cpp` — mesmo espírito de wrapper fino pro
  `amqp_client`.
- `src/services/sensor_simulator_service/AppSensorSimulatorService.cpp` — payload JSON
  já usado (reaproveitar o mesmo formato), padrão de thread periódica.
- `src/shared/repository/DeviceCredentialRepository.h/.cpp` — modelo pro
  `findAllActive()`.
- `src/shared/repository/SensorReadingRepository.h/.cpp` — modelo pro `insert()`.
- `src/third_party/CMakeLists.txt` — padrão FetchContent (URL + timeout) pro rabbitmq-c.
- `src/services/persistence_service/AppPersistenceService.cpp` — padrão de serviço sem
  capabilities HTTP-facing (mais próximo do `ReadingWriterService`, mas este ainda tem
  capabilities; `ReadingWriterService` não tem nenhuma).

## Verificação
- Migration V8 testada via Flyway local + `INSERT ... ON CONFLICT DO NOTHING` manual
  confirmando idempotência (segunda inserção da mesma `(sensor_id,timestamp)` não
  duplica).
- `device_credential.list_active`: teste unitário + `curl /invoke/` manual.
- RabbitMQ local via `docker run rabbitmq:3-management` avulso (sem esperar o compose
  novo) para validar `AmqpProducer`/`AmqpConsumer` isoladamente primeiro.
- Ponta a ponta real (o que faltava desde a sessão anterior): subir Postgres + gateway +
  device_service + RabbitMQ + `IngestionService` + `ReadingWriterService` +
  `SensorSimulatorService` (apontando pro host/porta real do `IngestionService` em vez
  do servidor de teste ad-hoc) — confirmar que uma leitura gerada pelo simulador chega
  em `sensor_readings` no Postgres.
- Suíte completa (`ctest`) sem regressão, como nas sessões anteriores.

## Microtarefas (tracking)

1. Migration `V8__sensor_readings_unique.sql` (`UNIQUE(sensor_id, timestamp)`)
2. `SensorReadingRepository::insert()` (idempotente, `ON CONFLICT DO NOTHING`)
3. `DeviceCredentialRepository::findAllActive()`
4. Capability `device_credential.list_active` no `device_service`
5. Vendorizar `rabbitmq-c` via FetchContent (`src/third_party/CMakeLists.txt`)
6. Wrapper `src/shared/amqp/amqp_client.h/.cpp` (`AmqpProducer`/`AmqpConsumer`)
7. `docker-compose.qa-mq.yml` (RabbitMQ + management/Prometheus plugin)
8. Novo serviço `IngestionService` — scaffolding + servidor CoAP/DTLS (libcoap, PSK)
9. `IngestionService` — thread de refresh de credenciais (poll 60s via
   `device_credential.list_active`)
10. `IngestionService` — handler de request: parse + validação mínima de formato +
    publish no RabbitMQ (uma mensagem por leitura)
11. Novo serviço `ReadingWriterService` — worker puro (sem ServiceClient/gateway),
    consumidor RabbitMQ + insert em `sensor_readings`
12. Registrar os dois novos serviços em `src/services/CMakeLists.txt` +
    `docker-compose.qa-app.yml` (build/deploy)
13. Verificação ponta a ponta real: simulador → IngestionService → RabbitMQ →
    ReadingWriterService → Postgres
14. Atualizar `Plano_Ingestion.md` com progresso + pendências (validação
    device_config completa, invalidação de credencial via EventBus)

## Pendência a registrar no `Plano_Ingestion.md` ao final
- Validação completa contra `device_config` (schema/faixas plausíveis) no
  `IngestionService` — adiada nesta entrega (só validação mínima de formato). Definir
  nessa próxima iteração como um serviço stateless acessa `device_config` (nova
  capability interna, provavelmente).
- Invalidação de credencial em tempo real (`device_credential.changed` via EventBus) —
  adiada; `IngestionService` usa poll periódico (60s) em vez de bridge no gateway.

## Runbook (VS Code) — subir tudo via Tasks

Forma mais prática de rodar em dev, sem digitar comandos: `.vscode/tasks.json` já tem
uma task pronta pra cada peça. Para rodar uma task: **Ctrl+Shift+P** (ou
**Cmd+Shift+P** no Mac) → **Tasks: Run Task** → escolher pelo nome. Cada `run-*-service`
abre num painel de terminal dedicado (fica rodando em background — não trava a UI).

Ordem recomendada:

1. **`docker-up-dev-mq`** — sobe o RabbitMQ local (container `rdws_rabbitmq_dev`,
   descartável). Management UI em [http://localhost:15672](http://localhost:15672)
   (usuário/senha `rdws_dev`/`rdws_dev`).
2. **`run-gateway`** — sobe o broker/gateway.
3. **`run-farm-service`**, **`run-field-service`**, **`run-device-service`** — ou
   **`run-all-services`** pra subir esses + auth/sensor/sensor-reading/persistence de
   uma vez (não inclui ingestion/reading-writer/simulator, que pedem passos extras
   abaixo).
4. Criar um device simulado (com sensores) via `bruno/IoT Sensor API/Devices/Create
   Device.bru`, marcando `is_simulated: true` no corpo — ou via `curl`, ver passo 4 do
   runbook manual abaixo. Guarde o `id` retornado.
5. **`run-ingestion-service`** e **`run-reading-writer-service`**.
6. **`run-sensor-simulator-service`** — a task pede o `device_id` num prompt de texto
   (canto superior da tela) antes de iniciar.
7. Disparar via Bruno: `Sensor Simulator > Trigger Manual Send`, ajustando `devId` nos
   Params → Path.
8. Quando terminar: **`docker-down-dev-mq`** (e `docker-down-dev-observability`, se
   também tiver subido essa). Os `run-*-service` você para clicando no ícone de lixeira
   do painel de terminal correspondente, ou `Ctrl+C` nele.

**Se só quiser ver a geração de leituras em arquivo** (sem a fila/banco de verdade),
pule os passos 1, 5 e 8 — o simulador continua gerando e acumulando no arquivo mesmo
sem `IngestionService`, só a transmissão via CoAP/DTLS que vai falhar (log
`CoAP/DTLS transmission failed`, sem perder dado).

## Runbook (terminal) — subir o pipeline localmente e disparar via Bruno

Roteiro usado na verificação ponta a ponta desta implementação — comandos equivalentes
às tasks acima, pra quem preferir/precisar rodar via terminal (CI, outra IDE, etc.).
Assume build já feito (`cmake --build build`) e Postgres/RabbitMQ disponíveis (local
nativo ou containers avulsos — não depende do `docker-compose.qa-*.yml` para rodar
local/dev).

### 1. Banco e migrations
```bash
# Postgres já rodando (nativo ou container) em localhost:5432, db "rdws_dev" por ex.
docker run --rm -v "$(pwd)/db/migrations:/flyway/sql" flyway/flyway:10 \
  -url=jdbc:postgresql://localhost:5432/rdws_dev -user=<user> -password=<senha> \
  -baselineOnMigrate=true migrate
```

### 2. RabbitMQ (se ainda não estiver rodando)
```bash
docker run -d --name rabbitmq -p 5672:5672 -p 15672:15672 \
  -e RABBITMQ_NODENAME=rabbit@localhost \
  -e RABBITMQ_DEFAULT_USER=<user> -e RABBITMQ_DEFAULT_PASS=<senha> \
  rabbitmq:3-management
```

### 3. Gateway + serviços de apoio
Em terminais separados (ou background), todos apontando pro mesmo `DB_HOST/PORT/USER/
PASSWORD/NAME` e pro gateway em `tcp://localhost:8080`:
```bash
export DB_HOST=localhost DB_PORT=5432 DB_USER=<user> DB_PASSWORD=<senha> DB_NAME=rdws_dev
export RDWS_AUTH_MODE=none   # ou "jwt" com RDWS_JWT_SECRET, se preferir autenticado

./build/src/service_broker/service_gateway_http 8080 3001 /tmp/rdws_gateway.sock routes.json &

./build/bin/farm_service farm_001 localhost tcp://localhost:8080 &
./build/bin/field_service field_001 localhost tcp://localhost:8080 &

export CREDENTIAL_ENCRYPTION_KEY=<32 bytes exatos>  # ver .env.qa.example
./build/bin/device_service device_001 localhost tcp://localhost:8080 &
```
Confirmar que todos registraram: `curl http://localhost:3001/health`.

### 4. Um device simulado (se ainda não existir)
```bash
curl -X POST http://localhost:3001/invoke/farm.create -d '{"name":"f1"}'
curl -X POST http://localhost:3001/invoke/field.create -d '{"farm_id":"1","name":"fld1"}'
curl -X POST http://localhost:3001/invoke/device.create \
  -d '{"field_id":"<id>","type":"weather_station","is_simulated":true}'
# guardar o "id" do device retornado — é o --device-id do simulador
```
Adicionar sensores (`sensors` table) e, opcionalmente, um `transmissions_per_day` alto em
`device_configurations.config` pra não esperar o intervalo real durante testes manuais.

### 5. `IngestionService` + `ReadingWriterService` (opcional para só olhar geração de
arquivo; obrigatório para ver a leitura chegar em `sensor_readings`)
```bash
export RABBITMQ_HOST=localhost RABBITMQ_PORT=5672 RABBITMQ_USER=<user> RABBITMQ_PASSWORD=<senha>
./build/bin/ingestion_service ingestion_001 localhost tcp://localhost:8080 &

export DB_HOST=localhost DB_PORT=5432 DB_USER=<user> DB_PASSWORD=<senha> DB_NAME=rdws_dev
./build/bin/reading_writer_service &
```

### 6. `SensorSimulatorService`
```bash
export DB_HOST=localhost DB_PORT=5432 DB_USER=<user> DB_PASSWORD=<senha> DB_NAME=rdws_dev
export SIMULATOR_DATA_DIR=./sim_data
export SIMULATOR_CONTROL_PORT=9100
export INGESTION_HOST=127.0.0.1 INGESTION_PORT=5684   # omitir se pular o passo 5
./build/bin/sensor_simulator_service --device-id <id> --gateway tcp://localhost:8080 &
```
Confirma que está gerando: `cat sim_data/*.data` (arquivo JSON crescendo a cada ~30s).

### 7. Disparar o envio via Bruno
1. Abrir a coleção `bruno/IoT Sensor API/` no Bruno.
2. Selecionar o ambiente **local** (canto superior direito) — já define
   `simulatorBaseUrl: http://localhost:9100`.
3. Ir em **Sensor Simulator > Trigger Manual Send**.
4. Em **Params → Path**, ajustar `devId` pro id do device usado no passo 4/6.
5. **Send** — resposta esperada `202 {"status":"triggered"}`.

Sem o `IngestionService` rodando (passo 5 pulado), o disparo tenta o handshake CoAP/DTLS
e falha (log `CoAP/DTLS transmission failed`) mas os dados continuam acumulados no
arquivo — não perde nada, só não confirma envio.

### 8. Verificar que chegou no banco
```bash
psql -h localhost -U <user> -d rdws_dev -c "SELECT * FROM sensor_readings ORDER BY id DESC LIMIT 10;"
```
Disparar de novo (passo 7) deve inserir só as leituras novas — sem duplicar as já
processadas (constraint `UNIQUE(sensor_id, timestamp)`, `V8`).
