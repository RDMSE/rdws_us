Data: 2026-07-15

# Detalhamento de implementação — `SensorSimulatorService` + pré-requisitos

Este documento detalha a implementação do `SensorSimulatorService` (visão de alto nível
em `Plano_SensorSimulatorService.md`), incluindo os dois pré-requisitos que o cliente
CoAP/DTLS exige: `device_credentials` (`Plano_DeviceCredentials.md`) e `is_simulated`
em `devices`.

## Contexto

Com o passo 7 do `Plano_Deployment.md` (CI/CD) fechado, o próximo passo é o 8:
`SensorSimulatorService`, a ferramenta que gera carga simulada para validar o pipeline
de ingestão (`Plano_Ingestion.md`).

Implementar o cliente CoAP/DTLS de verdade (não um stub) exige fechar antes o mecanismo
de credenciais PSK por device descrito em `Plano_DeviceCredentials.md` (tabela
`device_credentials`, capabilities `device_credential.*`), que ainda não existe no
código. Sem ele, o simulador não tem como autenticar a conexão DTLS por device.

Este plano cobre três entregas em sequência, cada uma pré-requisito da próxima:
1. `device_credentials` (provisionamento/consumo de PSK).
2. `is_simulated` em `devices` (como o simulador identifica quais devices carregar).
3. `SensorSimulatorService` propriamente dito (geração, persistência em arquivo, API de
   controle, envio CoAP/DTLS).

**Atualização**: o `IngestionService` (servidor CoAP/DTLS) já foi implementado
(`src/services/ingestion_service/AppIngestionService.cpp`, PR #83) — deixou de ser
bloqueador. Ele espera `POST` com body JSON `{ "device_id", "readings": [{ "sensor_id",
"timestamp", "value", "unit"? }] }` e valida PSK via `coap_context_set_psk2` com lookup em
`device_credential.list_active` (cache 60s). A verificação ponta-a-ponta real (simulador
→ `IngestionService` → RabbitMQ) já é possível hoje, não só local/manual.

**Fora do escopo deste plano**:
- Evento `device_credential.changed` no EventBus (só o `IngestionService` precisa
  invalidar cache com isso — o `SensorSimulatorService` já busca a credencial ativa a
  cada ciclo via `device_credential.get_active`, sem cache, conforme o próprio
  `Plano_DeviceCredentials.md` §5 já resolve).
- Versionamento de KEK / grace period de rotação (já registrados como adiados no próprio
  `Plano_DeviceCredentials.md` §7 e "Pontos em aberto").

## Parte 1 — `device_credentials`

### Migration (`db/migrations/V6__device_credentials.sql`)
Tabela conforme `Plano_DeviceCredentials.md` §1:
```sql
CREATE TABLE device_credentials (
    id              BIGINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
    device_id       BIGINT NOT NULL UNIQUE REFERENCES devices(id),
    psk_identity    UUID NOT NULL DEFAULT gen_random_uuid() UNIQUE,
    psk_key_enc     BYTEA NOT NULL,
    status          VARCHAR(16) NOT NULL DEFAULT 'active',
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    rotated_at      TIMESTAMPTZ,
    revoked_at      TIMESTAMPTZ
);
```
`pgcrypto` já está habilitado (`V1__iot_schema.sql:5`), então `gen_random_uuid()` já funciona.

### Crypto: novo módulo `src/shared/crypto/`
Não existe nenhum código de AES/GCM no repo hoje (só HMAC-SHA256 para JWT em
`src/service_broker/Auth/AuthMiddleware.cpp`). Criar `credential_cipher.h/.cpp` seguindo
o mesmo layout de módulo de `src/shared/config/`:
- `encrypt(plaintext, key) -> bytes` / `decrypt(bytes, key) -> plaintext`, usando OpenSSL
  EVP AES-256-GCM (`EVP_aes_256_gcm()`, `EVP_EncryptInit_ex`/`EVP_DecryptInit_ex`),
  formato de saída `nonce(12) || ciphertext || tag(16)` concatenados, igual à decisão do
  plano.
- Chave lida de `CREDENTIAL_ENCRYPTION_KEY` (env var), seguindo o padrão de
  `getenv_str` já usado em `src/service_broker/service_gateway_http.cpp:17-18,53` para
  `RDWS_JWT_SECRET` — lida uma vez no bootstrap do `device_service`.
- OpenSSL já é dependência linkada via `service_broker/CMakeLists.txt:40,57`
  (`OpenSSL::Crypto`) — mover/duplicar `find_package(OpenSSL REQUIRED)` para o
  `CMakeLists.txt` raiz (chamada idempotente) para que `src/shared/crypto` e o novo
  `libcoap` (Parte 3) também consigam linkar `OpenSSL::Crypto`/`OpenSSL::SSL` sem
  depender da ordem de `add_subdirectory`.

### Repository (`src/shared/repository/DeviceCredentialRepository.h/.cpp`)
Mesmo padrão de `DeviceRepository` (`src/shared/repository/DeviceRepository.cpp`):
struct `DeviceCredential`, métodos `create`, `findActiveByDeviceId`, `rotate`, `revoke` —
`psk_key_enc` sempre passa pelo `credential_cipher` antes de tocar o banco (repository
nunca vê texto puro fora do decrypt no `get_active`).

### Capabilities — dentro do `device_service` (não um serviço novo)
Adicionar ao `identity.capabilities` em `AppDeviceService.cpp:140-146`:
`device_credential.provision`, `device_credential.get_active`, `device_credential.rotate`,
`device_credential.revoke`. **Não adicionar nenhuma dessas em `routes.json`** —
`routes.json`/`EventRouter` é uma allow-list explícita para o HTTP público; uma
capability ausente de lá simplesmente nunca é alcançável via HTTP, sem precisar de
nenhum flag "interno" adicional. Chamadas entre serviços continuam via
`ServiceClient::invoke(...)` (mesmo canal TCP/unix já usado, ver
`FieldServiceClient::exists()` como precedente de chamada interna,
`src/services/device_service/FieldServiceClient.cpp:22`).

- **`device_credential.provision`**: chamado atomicamente dentro do próprio
  `handleCreate` de `AppDeviceService.cpp` (mesmo processo, mesmo `PostgreSQLDatabase`),
  logo após `svc.create(...)` ter sucesso — envolvido em
  `db_.beginTransaction()`/`commitTransaction()`/`rollbackTransaction()`
  (`src/shared/database/idatabase.h:49-51`, já suportado pela interface, só não usado
  ainda por nenhum handler) para garantir que device + credencial nascem juntos ou
  nenhum dos dois. Retorna o PSK em texto puro **uma única vez**, no mesmo payload de
  resposta de `device.create` (campo adicional, ex. `psk_identity`/`psk_key`).
- **`device_credential.get_active`**: recebe `device_id`, retorna `psk_identity` +
  chave decriptada — só chamável internamente (`SensorSimulatorService` vai usá-la, ver
  Parte 3).
- **`device_credential.rotate`** / **`device_credential.revoke`**: espelham o padrão de
  `handleUpdate`/`handleDelete` já existentes (validação de id numérico, etc.).

## Parte 2 — `is_simulated` em `devices`

### Migration (`db/migrations/V7__device_is_simulated.sql`)
```sql
ALTER TABLE devices ADD COLUMN is_simulated BOOLEAN NOT NULL DEFAULT false;
```
Trigger de imutabilidade (mesmo padrão de trigger já usado em `V3`/`V4` para outras
invariantes) rejeitando `UPDATE` que mude `is_simulated` após o insert.

### `device_service`
- `DeviceCreate`/`Device` (struct em `DeviceRepository.h`) ganha campo `isSimulated`,
  aceito só em `device.create` (schema `DEVICE_CREATE_SCHEMA` em
  `src/service_broker/schemas/device_schemas.h`) — **não** adicionar em
  `DEVICE_UPDATE_SCHEMA`, replicando exatamente a mesma técnica já usada pra blindar
  `field_id` de updates.
- Nova query auxiliar (`DeviceRepository::findAllSimulated` ou filtro `is_simulated` em
  `findAll`) com `JOIN sensors` para o `SensorSimulatorService` carregar device + seus
  sensores numa passada.

## Parte 3 — `SensorSimulatorService`

Novo diretório `src/services/sensor_simulator_service/`, novo target CMake (seguindo
`src/services/sensor_service/CMakeLists.txt` como modelo), registrado em
`src/services/CMakeLists.txt` (`add_subdirectory` + entrada no `foreach(SVC ...)`).

### Conexões (design híbrido, diferente dos demais serviços)
- **Conexão direta ao banco** (`PostgreSQLDatabase`, mesma leitura de env vars que os
  outros serviços) para ler `devices`/`sensors`/`device_configurations` filtrados por
  `--device-id` (CLI obrigatório) + `is_simulated = true` — sem passar pelo gateway,
  conforme `Plano_SensorSimulatorService.md`.
- **`ServiceClient` com `identity.capabilities` vazio** (cliente puro, não expõe
  nenhuma capability) só para chamar `device_credential.get_active` via `invoke(...)`
  imediatamente antes de cada ciclo de envio — sem cache, conforme
  `Plano_DeviceCredentials.md` §5.

### Três threads
(mesmo padrão de `persistence_service`'s `flushThread_`, `AppPersistenceService.cpp` —
sleep-loop verificando `running.load()`, mais `std::mutex`/`std::deque` → aqui, arquivo +
mutex por device/sensor):
1. **Geração**: escreve `<device_id>_<sensor_id>.data` (JSON) periodicamente, valor
   plausível por `sensor_type`/faixa configurada. MVP: um valor por sensor por
   intervalo configurado (`device_configurations.config`); a diferenciação fina de
   amostragem interna vs. transmissão para vento (`wind_speed_avg`/`gust`/`direction`
   vetorial, `Plano_Ingestion.md` "Amostragem interna vs. taxa de transmissão") fica
   registrada como próxima iteração, não bloqueia esta entrega.
2. **Transmissão**: no horário agendado, pausa geração (lock mutex do arquivo), chama
   `device_credential.get_active`, abre uma nova conexão CoAP/DTLS (`CoapDtlsClient`,
   ver abaixo), envia, limpa arquivo, libera lock.
3. **`httplib::Server`** (mesmo padrão de thread dedicada de `HttpGateway`) expondo
   `POST /simulate/{device_id}/trigger` — só aceita o `device_id` fixado por CLI,
   dispara a transmissão imediatamente fora do agendamento.

### Cliente CoAP/DTLS (`src/shared/coap/coap_dtls_client.h/.cpp`)
Wrapper fino em cima do **libcoap** (biblioteca C), com PSK via
`coap_context_set_psk2`/`coap_dtls_spsk_info_t` — escolha já registrada em
`Plano_DeviceCredentials.md` §8. Uma conexão nova por device por ciclo (sem conexão
persistente), método `sendConfirmable(host, port, pskIdentity, pskKey, payloadBytes) ->
bool`.

### Nova dependência: libcoap via FetchContent
Em `src/third_party/CMakeLists.txt`, seguindo o padrão `URL` (tarball, não
`GIT_REPOSITORY` — mesmo raciocínio de timeout já aplicado a fmt/spdlog/httplib):
```cmake
set(ENABLE_DTLS ON CACHE BOOL "" FORCE)
set(DTLS_BACKEND "openssl" CACHE STRING "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libcoap
    URL https://github.com/obgm/libcoap/archive/refs/tags/v4.3.5.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TIMEOUT 60
    INACTIVITY_TIMEOUT 30
)
FetchContent_MakeAvailable(libcoap)
```
Confirmado via CMakeLists.txt real da tag `v4.3.5`: todas as opções acima existem
(`ENABLE_DTLS`, `DTLS_BACKEND`, `ENABLE_TESTS`, `ENABLE_EXAMPLES`, `ENABLE_DOCS`,
`BUILD_SHARED_LIBS`), com suporte CMake completo (não é autotools-only nessa tag) —
`git`/`cmake`/`build-essential`/`libssl-dev` já presentes no `Dockerfile` builder stage
(linhas 17-24) cobrem o build, sem pacotes novos no Dockerfile. Runtime stage não muda
(build estático, `BUILD_SHARED_LIBS OFF`).

## Arquivos de referência (reaproveitar padrão)
- `src/services/device_service/AppDeviceService.cpp` — capability registration/dispatch.
- `src/shared/repository/DeviceRepository.h/.cpp` — repository pattern.
- `src/services/persistence_service/AppPersistenceService.cpp` — padrão de thread
  periódica coexistindo com o loop do `ServiceClient`.
- `src/services/device_service/FieldServiceClient.cpp` — chamada interna via
  `ServiceClient::invoke`.
- `src/service_broker/schemas/device_schemas.h` + `capability_schema_registry` —
  schemas valijson por capability.
- `src/third_party/CMakeLists.txt` — padrão FetchContent (URL + timeout).

## Verificação
- Migrations: `flyway migrate` local (mesmo fluxo de `db/migrations`) aplica V6/V7 sem
  erro; trigger de imutabilidade testado via `UPDATE devices SET is_simulated = ...`
  (deve falhar).
- `device_credential.provision`/`get_active`/`rotate`/`revoke`: testes unitários no
  padrão dos demais handlers (`src/services/device_service` ou `src/shared/tests`),
  cobrindo o roundtrip encrypt/decrypt do `credential_cipher` e o fato de `psk_key_enc`
  nunca aparecer em texto puro fora do `get_active`.
- Confirmar via `grep` que nenhuma entrada `device_credential.*` foi adicionada em
  `routes.json` (inacessível via HTTP, só via `ServiceClient::invoke`).
- `SensorSimulatorService`: build local (`cmake --build build --target
  sensor_simulator_service`), rodar com `--device-id` de um device simulado seedado,
  confirmar geração de arquivo `.data`, disparo manual via
  `POST /simulate/{id}/trigger` (nova coleção Bruno, mesmo padrão de
  `bruno/IoT Sensor API/`).
- CoAP/DTLS: com o `IngestionService` já implementado, a verificação ponta-a-ponta real é
  possível — subir `ingestion_service` local com o mesmo PSK provisionado, disparar o
  envio pelo `SensorSimulatorService` e confirmar handshake DTLS + payload aceito (`2.04
  Changed`) + mensagem publicada em `sensor_readings` no RabbitMQ.
