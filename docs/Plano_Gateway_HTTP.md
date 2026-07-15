# Plano de evolução do Gateway HTTP (Service Broker)

Data: 2026-05-15 | Atualizado: 2026-06-10

## Objetivo
Consolidar o gateway HTTP em C++ que recebe requisições, transforma em payload de evento Lambda e roteia para o microserviço capaz de atender a capability solicitada.

## Diagnóstico atual do código

### Fluxo já implementado
1. Entrada HTTP no endpoint POST /invoke/{capability}.
2. Autenticação via AuthMiddleware (API key ou JWT HS256); identidade injetada no LambdaContext.
3. Resolução de capability via EventRouter (roteamento dinâmico com condições e fallback).
4. Conversão da requisição para payload com:
   - lambdaEvent (estrutura derivada de LambdaEvent)
   - lambdaContext (estrutura derivada de LambdaContext, com identidade do caller)
   - metadados HTTP (headers, queryStringParameters, pathParameters, body, capability)
5. Roteamento no gateway via selectBestService(capability) com estratégia configurável (LEAST_LOADED padrão).
6. Envio para microserviço por socket (tcp/unix) com framing explícito de mensagem.
7. Modo síncrono: gateway aguarda RESPONSE por até N ms (timeout por capability via GatewayConfig).
8. Modo assíncrono: retorna 202 + requestId; resultado disponível em GET /requests/{requestId}.
9. Cleanup automático de requests terminais após 5 minutos de retenção.

### Endpoints implementados
- POST /invoke/{capability} — despacha request (sync ou async)
- GET /requests/{requestId} — consulta resultado de request assíncrono
- GET /status — estado geral do gateway
- GET /connections — serviços conectados
- GET /metrics — métricas por capability (count, avgLatency, p99, errorRate)
- GET /health — saúde do gateway com uptime e stats
- GET /routes — lista regras do EventRouter
- POST /routes — adiciona regra de roteamento
- GET /routes/{id} — consulta regra
- PUT /routes/{id} — atualiza regra
- DELETE /routes/{id} — remove regra

### Decisões de design tomadas
1. **Modelo híbrido async+sync**: waitForResponse com timeout; se estourar, retorna 202 com requestId.
2. **TTL de resultado**: requests terminais (completed/failed/timeout) são removidos após 5 minutos de retenção.
3. **Load balancing padrão**: LEAST_LOADED; configurável por capability (round_robin, fastest_response, random).

---

## Histórico de execução (fases concluídas)

### Fase 0 — Adicionar RapidJSON e tl/expected ao third_party ✅
- Adicionar RapidJSON via FetchContent (vendored do valijson por compatibilidade com GCC 14).
- Adicionar `tl::expected` via FetchContent como alternativa a exceções.

### Fase 0b — Migrar jsoncpp → RapidJSON em todo o projeto, remover jsoncpp_bundled ✅
- Substituir todos os usos de `jsoncpp` por RapidJSON.
- Remover target `jsoncpp_bundled` do CMake.

### Fase 1 (fundação) — Portar LambdaEvent, LambdaContext, ServiceResult, LambdaParamsHelper ✅
- Portar tipos do `rdws_webserver` para `src/shared/types/`.
- Estabelecer contrato canônico de evento usado pelo HttpGateway.

### Fase 2 (fundação) — Estender config e validator existentes ✅
- Ampliar `src/shared/config/` e `src/shared/validator/` para suportar os novos tipos.

### Fase 3 (fundação) — Adicionar cpp-httplib ✅
- Integrar `cpp-httplib v0.15.3` via FetchContent como servidor HTTP header-only.

### Fase 4 (fundação) — HttpGateway com contrato LambdaEvent/LambdaContext ✅
- Implementar `HttpGateway` com `POST /invoke/{capability}`, `GET /status`, `GET /connections`.
- Renomear `ServiceBroker` → `ServiceGateway` em todo o projeto.

---

## Plano de execução por fases

### Fase 1 - Estabilização do protocolo ✅
- Implementar framing de mensagem no canal broker<->service.
- Implementar armazenamento real de pendingRequests com timestamp e estado (queued, in_flight, completed, failed, timed_out).
- Implementar cleanupExpiredRequests com retenção de 5 minutos.
- Critério de aceite: sem perda de mensagens em testes com payloads maiores e bursts. ✅

### Fase 2 - Ciclo de resposta completo ✅
- Correlacionar RESPONSE ao requestId.
- Persistir resultado em estrutura de request tracking.
- Criar endpoint GET /requests/{requestId}.
- Critério de aceite: requisição HTTP consegue recuperar resultado processado. ✅

### Fase 3 - Modo síncrono opcional ✅
- Adicionar opção waitForResponse com timeout configurável.
- Retornar resposta do microserviço diretamente quando disponível.
- Critério de aceite: smoke test cobrindo timeout, sucesso e erro do serviço. ✅

### Fase 4 - Observabilidade e operação ✅
- ✅ Métricas por capability: latência (avg, p99, min, max), taxa de erro, contagem de timeouts — `MetricsTracker` com ring buffer 200 amostras.
- ✅ Logs estruturados JSON (spdlog) com requestId ponta a ponta: `http_request`, `http_response`, `response_correlated`, `service_connected`, `service_disconnected`.
- ✅ Endpoint `GET /metrics` — per-capability `requestCount`, `avgLatencyMs`, `p99LatencyMs`, `errorRate`.
- ✅ Endpoint `GET /health` — `status`, `uptimeEpochSec`, gateway stats (`activeConnections`, `pendingRequests`), per-service summary.
- ✅ Log file rotativo opcional via 4º argumento CLI (`logFile`).
- Critério de aceite: troubleshooting por requestId sem inspeção manual de socket. ✅

### Fase 5 - Testes e hardening ✅
- ✅ Testes unitários de ServiceGateway (ciclo request/response, timeout, desconexão de serviço).
- ✅ Testes unitários de métricas, auth middleware, EventRouter e EventBus.
- ✅ Testes HTTP end-to-end (`test_http_e2e.cpp`): cliente HTTP real → HttpGateway → ServiceClient mock → resposta HTTP.
  - Endpoints de observabilidade (status, health, metrics, connections) → 200.
  - POST /invoke sem serviço disponível → 503.
  - Echo service responde com payload → 200.
  - Serviço lento excede timeout configurado → 504.
  - Auth por API key: chave ausente → 401; chave válida → 200.
  - GET /requests/{id} com id inexistente → 404.
  - EventRouter redireciona capability de entrada para capability de saída com serviço ativo → 200.
- ⬜ Testes de carga leve com múltiplas capabilities simultâneas.
- Critério de aceite: suíte cobrindo cenários críticos de regressão no stack HTTP completo. ✅ (parcial)

### Fase 6 - EventRouter ✅
- ✅ Roteamento dinâmico de capability com condições (eq, ne, contains, exists) sobre payload.
- ✅ Fallback capability quando outputCapability não tem serviço disponível.
- ✅ CRUD de regras exposto via HTTP (GET/POST/PUT/DELETE /routes).
- ✅ Persistência de regras em arquivo JSON configurável.
- ✅ Testes unitários cobrindo resolução, condições e prioridade.

### Fase 7 - EventBus ✅
- ✅ Canal pub/sub interno assíncrono com worker em background.
- ✅ API subscribe/unsubscribe/publish thread-safe.
- ✅ Testes unitários cobrindo publicação, múltiplos subscribers e unsubscribe.

### Fase 8 - Auth ✅
- ✅ Autenticação por API key (constant-time comparison) e JWT Bearer (HS256 HMAC).
- ✅ Propagação de identidade (subject, issuer, claims) para LambdaContext via `injectIdentity`.
- ✅ Caminhos públicos configuráveis (bypass de auth).
- ✅ Testes unitários cobrindo API key válida/inválida, JWT válido/expirado/assinatura incorreta.

### Fase 9 - Config & feature flags ✅
- ✅ Timeout por capability com fallback para global default.
- ✅ Estratégia de load balancing por capability (least_loaded, round_robin, fastest_response, random).
- ✅ Limite de concorrência por capability (`maxConcurrency`).
- ✅ Feature flags booleanas por nome (`isEnabled(feature)`).
- ✅ Carregamento e persistência de config via arquivo JSON.
- ✅ API HTTP para leitura/atualização de config em runtime — `GET /config` (snapshot completo), `PATCH`/`DELETE /config/capabilities/{cap}` (override e revert por capability), `GET /config/features` e `PUT /config/features/{name}` (feature flags), em `HttpGateway.cpp`.

### Fase 9b - AuthService e emissão de JWT

**Decisão de design:** um microserviço dedicado (`AuthService`) é responsável por validar credenciais e emitir tokens JWT. Ele conecta diretamente ao banco de usuários (tabela `users`). O gateway já valida o Bearer token nas requisições subsequentes (fase 8) — a fase 9b adiciona apenas o mecanismo de emissão.

**Fluxo:**
```
POST /auth/login → gateway → AuthService → valida credenciais no banco → retorna JWT Bearer
```

**Endpoint:**

| Método | Path          | Capability   | Descrição                              |
|--------|---------------|--------------|----------------------------------------|
| POST   | /auth/login   | auth.login   | Recebe credenciais, retorna JWT Bearer |

**Payload de request:**
```json
{
  "body": {
    "username": "joao",
    "password": "senha"
  }
}
```

**Payload de response:**
```json
{
  "token": "<jwt>",
  "expiresAt": "2026-06-18T00:00:00Z"
}
```

**Claims do JWT emitido:**
- `sub` — id do usuário
- `username` — nome do usuário
- `role` — perfil de acesso (ex: `admin`, `operator`, `viewer`)
- `iat` / `exp` — emitido em / expira em

**Observações:**
- `/auth/login` é caminho público no gateway (bypass de auth — já suportado na fase 8).
- Password armazenado com hash (`bcrypt` ou `argon2`) — nunca em texto plano.
- `role` é incluído no JWT mas o controle de acesso por role será implementado em fase futura.
- `AuthService` é pré-requisito para a fase 10 (banco), pois o banco de usuários faz parte do mesmo schema.

- ✅ Criar tabela `users` via migration Flyway (ver `Plano_DB_IOT_Sensors.md`).
- ✅ Implementar `AuthService` com validação de credenciais e emissão de JWT HS256.
- ✅ Registrar `/auth/login` como caminho público no gateway.
- ✅ Adicionar capability `auth.login` no EventRouter.
- Critério de aceite: POST /auth/login com credenciais válidas retorna JWT aceito pelo gateway nas requisições seguintes; credenciais inválidas retornam 401. ✅ (testado)

---

### Fase 10 - DB/Flyway e CI/CD

#### 10a — Persistência via PersistenceService

**Decisão de design:** o gateway não escreve direto no banco. Um microserviço dedicado (`PersistenceService`) se conecta ao gateway pelo socket existente (mesmo protocolo dos demais microserviços) e subscreve eventos do EventBus interno. Se o PersistenceService cair, o gateway continua operando sem degradação.

**Fluxo:**
```
Gateway (EventBus) ──publica──> request.completed / metrics.snapshot
PersistenceService ──subscreve──> acumula em buffer interno ──batch upsert──> PostgreSQL
```

**Eventos publicados pelo gateway:**
- `request.completed` — requestId, capability, latencyMs, status (completed/failed/timeout), timestamp
- `metrics.snapshot` — emitido por capability a cada N requests ou intervalo T (a definir)

**Tabelas no PostgreSQL:**
- `request_history` — histórico de requests com status e latência
- `capability_metrics` — métricas agregadas por capability e janela de tempo

**Migrações:** gerenciadas via **Flyway** com versionamento em `db/migrations/`.

**Política de limpeza (cleanup):**
- `request_history`: retenção de 90 dias; job periódico (`pg_cron` ou processo externo) remove registros antigos.
- `capability_metrics`: registros brutos retidos 30 dias; agregações diárias/semanais mantidas por 1 ano.

- ✅ Definir e publicar eventos `request.completed` e `metrics.snapshot` no EventBus do gateway.
- ✅ Implementar `PersistenceService` com buffer interno e batch upsert no PostgreSQL.
- ✅ Criar migrations Flyway para `request_history` e `capability_metrics`.
- ✅ Implementar job de cleanup de registros antigos — serviço `db_cleanup` (`postgres:16-alpine`, só `psql`) adicionado em `docker-compose.qa-db.yml` e `docker-compose.dev-db.yml`: loop `while true` chamando `cleanup_old_request_history()`/`cleanup_old_capability_metrics()` a cada 24h (1x no start + `sleep 86400`). Sem `pg_cron`: não vem na imagem `postgis/postgis:16-3.4`, exigiria build de imagem própria só pra isso — granularidade diária já atende a política de retenção em dias. Testado de ponta a ponta contra o Postgres real de dev (fedora-server): funções executam sem erro e não apagam nada fora da janela de retenção (dados com no máx. 5 dias).
- ✅ Adicionar `PersistenceService` como datasource no Grafana — datasource Postgres provisionado via arquivo (`infra/grafana/provisioning/datasources/datasources.yml`, uid `rdws-persistence`), credenciais injetadas via `$__env{...}` (compartilhando o mesmo `x-db-env` do QA). Testado de ponta a ponta contra o Postgres real de dev no fedora-server: `health` OK e query real em `capability_metrics`/`request_history`.
  - Achado no teste: sintaxe de interpolação errada na primeira tentativa (`$ENV{...}` não existe; Grafana usa `$__env{...}`).
  - Achado no teste: Grafana de dev roda em container e precisa resolver `fedora-server` via Tailscale MagicDNS — rede bridge comum não herda esse DNS do host. Tentativa inicial com `network_mode: host` resolveu o Postgres mas quebrou a resolução dos containers vizinhos (prometheus/loki), deixando os painéis desses datasources vazios. Fix definitivo: manter rede bridge e apontar `dns: [100.100.100.100]` (resolver MagicDNS do Tailscale) no serviço `grafana`, que preserva a resolução interna do Compose.
  - Painéis adicionados ao dashboard `gateway-overview.json` usando o datasource `PersistenceService` (histórico independente do gateway estar rodando/scraped): requests por capability, latência avg/p99, tabela de request history recente.
- Critério de aceite: queda do PersistenceService não afeta gateway; request history e métricas consultáveis no banco após reconexão. ✅ (corrigido e testado em 2026-07-08 — ver bugs abaixo; o critério original estava marcado ✅ mas não tinha sido validado de ponta a ponta contra reconexão real)

**Quatro bugs reais encontrados e corrigidos no primeiro deploy real em QA (2026-07-08/09)**
— nenhum pego pelos testes existentes, porque nenhum cobria o ciclo completo
gateway↔EventBus↔PersistenceService com dados reais, reconexão de serviço, nem o boot do
container com volume vazio:

1. **Bridge `request.completed`/`metrics.snapshot` não injetava `capability` no payload.**
   `ServiceGateway::start()` encaminha o evento cru como payload da chamada
   `persistence.save.request`/`persistence.save.metrics`, mas todo serviço despacha a
   capability lendo `request["capability"]` (`ServiceClient::handleRequest` só repassa
   `message["data"]`, descartando o `capability` do envelope de fio). Fluxos HTTP normais
   funcionam porque `HttpGateway` injeta `capability` manualmente no payload — o bridge
   interno não fazia o mesmo. Resultado: 100% dos eventos bridged eram rejeitados como
   "Unknown capability" (para `request.completed`, o payload *tinha* um campo
   `capability`, mas era o da requisição original, ex. `farm.list` — nunca batia com
   nenhum handler do PersistenceService). Corrigido injetando `capability` corretamente
   nos dois bridges (renomeando o campo original pra `originalCapability` no caso de
   `request.completed`, já que `handleSaveRequest` precisa dele pra gravar no histórico).
2. **`PersistenceService::flushMetricsBuffer` esperava o formato errado de JSON.**
   `metrics.snapshot` tem a forma `{"capabilities": [...], "snapshotAt": "..."}` (array),
   mas o parser iterava os membros do documento como se fosse um objeto plano
   `{"cap": {...}}` — `"capabilities"` é array (`IsObject()` retorna falso), então o loop
   nunca executava nenhum `INSERT`, silenciosamente (sem exceção, sem erro nos logs,
   `capability_metrics` ficava vazia pra sempre).
3. **`ServiceGateway::closeConnection` nunca removia a entrada de `activeConnections`
   nem fechava o file descriptor no SO — o mais grave dos três.** Ao desconectar, a
   entrada antiga (com `identified=true` e o mesmo `serviceId`) permanecia no
   `std::map<int, ClientConnection>` pra sempre. Numa reconexão do mesmo serviço,
   `sendDirectRequest` (que itera o map em ordem ascendente de fd e usa o primeiro match)
   quase sempre encontrava a entrada **antiga e morta** antes da nova — todo request
   subsequente pra esse serviço era roteado silenciosamente pro fd morto. Isso explica
   por que o problema só aparece **depois** de pelo menos uma reconexão (deploy,
   restart, crash) — a primeira conexão de vida do processo funciona normalmente.
   Reproduzido de forma determinística rodando gateway + PersistenceService nativos
   (fora de container) e forçando um disconnect/reconnect; confirmado com `gdb -p <pid>
   -batch -ex "thread apply all bt"` (nenhuma thread em deadlock — o bug não trava nada,
   só rotea mensagens pro buraco) e logs de instrumentação temporários mostrando
   `activeConnections.size() == 2` e o fd escolhido sendo sempre o antigo. Corrigido:
   `closeConnection` agora chama `activeConnections.erase(it)` e `::close(clientFd)`.
   Validado com 4 ciclos consecutivos de `metrics.snapshot` pós-reconexão, todos
   `state=completed`.
4. **`routes.json` do EventRouter nunca era carregado em container — encontrado ao
   testar `POST /auth/login` via Bruno contra a QA real.** O `Dockerfile` copia o
   `routes.json` do repo pra `/app/routes.json` na imagem, mas
   `docker-compose.qa-app.yml` manda o gateway usar `/app/data/routes.json` (dentro do
   volume `gateway_data`, que começa **vazio** num volume novo). `EventRouter::loadFromFile`
   falha silenciosamente se o arquivo não existe (`rules_` fica vazio, sem fallback) —
   todo path REST definido em `routes.json` (`POST /auth/login`, `GET /farms`, etc.)
   respondia 404 "No route found", mesmo com as regras corretas embutidas na imagem.
   Só `/invoke/{capability}` (usado em toda a validação desta sessão) funcionava, o que
   escondeu o bug até alguém testar a rota REST "bonita" de verdade. Corrigido com
   `docker-entrypoint.sh`: semeia o volume com o `routes.json` da imagem só se o arquivo
   ainda não existir (idempotente — CRUD de rotas em runtime via `/routes` continua
   persistindo no volume normalmente entre redeploys, sem risco de sobrescrita).

**Nota:** o bug nº3 potencialmente afeta *qualquer* serviço que reconecta (não só
PersistenceService) — vale considerar se o hang observado no `ctest` do runner
self-hosted (Fase 10b) tinha relação, embora não tenha sido confirmado.

**5º achado (2026-07-09), fora do ciclo de bugs acima — esgotamento do thread pool do
gateway HTTP.** Encontrado testando timeout de serviço lento via Bruno: cada handler de
`/invoke`/REST fica bloqueado em `ServiceGateway::waitForResponse()` até o timeout da
capability (30s default), mesmo depois do cliente HTTP desistir e fechar a conexão — o
`cpp-httplib` não avisa o handler que o cliente saiu (só detecta isso em respostas
streamed/chunked, não em handlers síncronos normais como os nossos). Retries rápidos
contra um serviço travado esgotavam o pool padrão do httplib (`max(8, cores-1)`), e
novas conexões ficavam sem thread livre — sintoma: gateway "não aceita" a próxima
requisição. Mitigado (não resolvido na raiz) aumentando o pool explicitamente pra 64
threads (`HttpGateway::HttpGateway`, `server_.new_task_queue`). A correção de raiz
(abortar a espera assim que o cliente desconecta, liberando a thread na hora) exigiria
reestruturar o handler pra usar resposta streamed/chunked — backlog, só se o pool maior
não for suficiente na prática.

---

#### 10b — CI/CD (GitHub Actions + self-hosted runner + Docker)

**Decisão de design:** GitHub Actions com self-hosted runner no servidor Fedora doméstico. Build e testes rodam dentro de container Docker para ambiente reproduzível (GCC, CMake, dependências). Deploy é substituição do container em execução no mesmo servidor.

**Pipeline:**
```
push/PR → build Docker → testes unitários + e2e → (merge main) → deploy container
```

- ✅ Criar `Dockerfile` de build multi-stage (builder com GCC/CMake → imagem final mínima).
  - Base `ubuntu:24.04` (não Debian bookworm): bookworm só tem `libpqxx-dev` 6.4.5, e o
    código (`postgresql_database.cpp`) usa API do libpqxx 7 (`pqxx::params`,
    `exec_prepared`). Ubuntu 24.04 tem `libpqxx-dev` 7.8.1, compatível.
  - Corrigidos de passagem includes faltantes que só compilavam por inclusão transitiva
    no GCC local (`<optional>` em `GatewayConfig.h`, `<iomanip>`/`<ctime>` em
    `ServiceMonitor.cpp`, `<array>` em `AuthMiddleware.cpp`) e troca de
    `pqxx::prepped{...}` (não existe no 7.8) por `exec_prepared(...)`.
  - Validado localmente: build do estágio `builder` compila, 134 testes passam via
    `ctest` dentro do container, e o binário do estágio `runtime` sobe e responde em
    `GET /health` e `GET /status`.
- ✅ Instalar e registrar self-hosted runner no servidor Fedora (labels `self-hosted,
  homelab, docker, embedded`; serviço systemd em `/opt/actions-runner`).
- ✅ Criar workflow `ci.yml`: builda o estágio `builder` (compila + `ctest`) e o estágio
  `runtime` completo em todo push/PR, rodando no runner self-hosted.
  - **Validado em produção (2026-07-08)**: PR real pra branch `dev` disparou o workflow
    no runner self-hosted (`fedora-server`) — primeira run falhou (`actions/checkout@v4`
    não inicializa git submodules por padrão; `src/third_party/{inih,valijson,
    dotenv-cpp}` são submodules, ficavam vazios no runner → cmake configure quebrava com
    "does not contain a CMakeLists.txt file"). Corrigido com `submodules: recursive` no
    step de checkout. Segunda run: build + 134 testes + imagem runtime, tudo verde.
- ✅ Criar workflow `deploy-qa.yml`: dispara em `workflow_run` do CI (sucesso na main) ou
  manualmente (`workflow_dispatch`); gera `.env.qa` a partir do Environment `qa`, roda
  Flyway, sobe `docker-compose.qa-app.yml --build`, valida `GET /health` com retry, limpa
  `.env.qa` no final (`if: always()`).
- ✅ Configurar secrets/vars no GitHub — Environments `development` e `qa` criados com
  secrets (`DB_HOST`, `DB_PORT`, `DB_USER`, `DB_PASSWORD`, `DB_NAME`, `RDWS_JWT_SECRET`) e
  vars (`LOG_LEVEL`, `PORT`, `RDWS_AUTH_MODE`, `RDWS_ENVIRONMENT`, `RDWS_JWT_ISSUER`).
  - Achado: `deploy-qa.yml` gerava `RDWS_JWT_ISSUER=` vazio fixo no `.env.qa`, ignorando a
    var configurada — validação de issuer não tinha efeito nenhum no deploy real. Corrigido
    pra ler de `${{ vars.RDWS_JWT_ISSUER }}`.
  - Achado: `RDWS_JWT_AUDIENCE` fica de fora de propósito — GitHub não aceita variável com
    valor vazio na UI, e um valor não-vazio (ex. espaço) quebraria a validação de audience
    pra todo mundo (`AuthMiddleware::jwtAudience.empty()` deixaria de pular o check). O
    fallback vazio do compose (`${RDWS_JWT_AUDIENCE:-}`) já cobre o caso de não validar.
  - Backlog: `RDWS_AUTH_MODE` e `RDWS_ENVIRONMENT` estão hardcoded em
    `docker-compose.qa-app.yml` (não lidos de env) — as vars com esses nomes no GitHub
    hoje não têm efeito. `LOG_LEVEL`/`PORT` também não chegam aos serviços (cai no default
    `"info"` do código). Conectar via env se algum dia precisar variar por ambiente; sem
    isso o comportamento atual em QA já está correto.
- Critério de aceite: PR abre → CI roda automaticamente ✅ (validado); merge na main →
  novo container em produção sem intervenção manual ✅ (validado em 2026-07-14 — merge na
  main disparou `deploy-qa.yml` via `workflow_run`, deploy passou).

**Labels do runner self-hosted (registrado em 2026-07-06):** `self-hosted, homelab, docker, embedded`. O label `embedded` foi adicionado antecipando um futuro firmware para os sensores/dispositivos — o mesmo runner poderá compilar toolchains embarcadas sem precisar ser reconfigurado. Workflows devem usar `runs-on: [self-hosted, homelab, docker]` (ou incluir `embedded` quando houver jobs de firmware).

**Instalação como serviço systemd — nota SELinux:** o runner precisa ficar em `/opt/actions-runner` (não em `~/tools/actions-runner` ou qualquer path dentro do home do usuário). No Fedora, SELinux enforcing bloqueia o `systemd` de executar `runsvc.sh` quando o diretório está em contexto `user_home_t`, mesmo com `chmod +x` (erro `status=203/EXEC` / `Permission denied` no `journalctl`). Diagnóstico: `getenforce` + `sudo ausearch -m avc -ts recent | grep runsvc`. Para mover um runner já configurado sem gerar token novo, copiar a pasta inteira (`.runner`, `.credentials`, `.credentials_rsaparams` etc.) com `rsync -a` para o novo path e rodar `svc.sh install`/`start` de lá — **não** rodar `config.sh` de novo (token só é necessário na primeira configuração).

### Fase 11 - Observabilidade de Logs (Loki + Grafana)

**Decisão de design:** logs não vão para o banco. O gateway já emite logs estruturados JSON via spdlog com arquivo rotativo. O Promtail lê esse arquivo e envia para o Loki; o Grafana consome ambos (PostgreSQL para métricas/requests, Loki para logs).

- ✅ Configurar **Loki** como datasource de logs — `infra/loki/loki-config.yml`.
- ✅ Configurar **Promtail** apontando para o log file rotativo do gateway (zero mudança no código C++) — `infra/promtail/promtail-{dev,qa}.yml`, validado com dados reais fluindo.
- ✅ Adicionar datasource Loki no Grafana — `infra/grafana/provisioning/datasources/datasources.yml` (uid `rdws-loki`).
- ✅ Criar dashboard no Grafana unificando métricas (Prometheus), logs (Loki) e histórico
  (PersistenceService/PostgreSQL) — `gateway-overview.json`.
- ✅ Definir política de retenção no Loki (período e compressão) — `loki-config.yml` agora tem
  `limits_config.retention_period: 720h` (30 dias) e `compactor` com `retention_enabled: true`
  (`delete_request_store: filesystem`, `retention_delete_delay: 2h`); compressão de chunks
  via `ingester.chunk_encoding: snappy`. Validado em QA: compactor sobe e aplica retenção
  (`applying retention with compaction`), sem erros nos logs.
- Critério de aceite: dado um `requestId`, conseguir navegar do painel de métricas até os
  logs daquele request no Grafana. ⬜ — painéis existem lado a lado no mesmo dashboard, mas
  sem derived fields/data link entre eles; hoje é preciso copiar o `requestId` e filtrar
  manualmente no painel de Logs.

---

### Fase 12 - Profiling granular via RequestContext

**Contexto:** o `Profiler` atual mede o tempo total do handler no `processRequest`. Para granularidade maior (DB, serialização, cálculos), seria necessário propagar o profiler pelo call stack.

**Decisão de design proposta:**
- Introduzir `struct RequestContext { const rapidjson::Document& req; Profiler& profiler; }` — pode crescer com identity JWT, trace ID, etc.
- `CapabilityHandler<TService>` passa a receber `RequestContext` em vez de `const rapidjson::Document&`.
- Handlers fazem `ctx.profiler.scoped("db.query")`, `ctx.profiler.scoped("json.serialize")`, etc.

**Quando faz sentido:** serviços com cálculos complexos, queries lentas para investigar, ou base de dados grande onde a origem do tempo importa.

- ⬜ Definir `struct RequestContext` em `capability_router.h`.
- ⬜ Atualizar `CapabilityHandler` e `dispatchCapability` para usar `RequestContext`.
- ⬜ Migrar handlers de todos os serviços para a nova assinatura.
- Critério de aceite: dump do profiler mostra breakdown por etapa (db, serialize, etc.) em vez de só o tempo total.

---

### Fase 13 - Segurança: prevenção de IDOR

**Contexto:** os IDs expostos na API são `BIGSERIAL` sequenciais, permitindo enumeração trivial de recursos (IDOR — Insecure Direct Object Reference).

**Decisão de design proposta:**
- Adicionar coluna `public_id UUID DEFAULT gen_random_uuid()` nas tabelas expostas (`farms`, `fields`, `devices`, `sensors`, `sensor_readings`, `device_configurations`).
- A API passa a expor e receber `public_id` em vez do `id` interno.
- Repositórios fazem lookup por `public_id` nas queries.
- Complementar com validação de ownership pelo `sub` do JWT nos handlers (filtrar por tenant/usuário).

- ⬜ Migration adicionando `public_id UUID` nas tabelas afetadas (com índice único).
- ⬜ Atualizar repositórios para buscar/retornar `public_id`.
- ⬜ Atualizar serialização (`xToJson`) para expor `public_id` como `id`.
- ⬜ Atualizar handlers para receber `public_id` nos path params.
- ⬜ Atualizar seed de dados fake.
- ⬜ Validação de ownership nos handlers usando `lambdaContext.identity.claims`.
- Critério de aceite: nenhum `BIGSERIAL` interno exposto na API; acesso cruzado entre usuários retorna 403.

---

### Fase 13b - Validação de input (POST/PUT) contra schema

**Contexto:** hoje cada handler valida campo a campo na mão (ex.: `handleUpdate` do
`device_config_service` só checa se `config` está vazio) — inconsistente entre serviços
e fácil de esquecer um campo obrigatório ou tipo errado. O projeto já tem a infra pra
schema validation (`valijson` + `rapidjson`, `src/shared/validator/schema_validator.h`)
desde a Fase 0/0b, mas ela não está conectada no pipeline de request — `src/service_broker/schemas/`
existe mas está vazio.

**Decisão de design proposta:**
- Um arquivo de schema JSON por capability de escrita (`farm.create`, `farm.update`,
  `device_config.update`, etc.) em `src/service_broker/schemas/`.
- Validação acontece no `HttpGateway` (ou no `dispatchCapability`/`capability_router.h`,
  a decidir) antes de rotear pro serviço — request malformado nunca chega no backend,
  retorna 400 com mensagem de validação clara direto do `SchemaValidator`.
- Reaproveita o `ServiceResult`/`OperationResult` (ver decisão da sessão de 2026-07-09)
  pro formato de erro devolvido.

- ✅ (2026-07-14) Definidos schemas JSON (draft-07) pras 9 capabilities de escrita
  (`farm.create/update`, `field.create/update`, `device.create/update`,
  `sensor.create/update`, `device_config.update`) — embutidos como `const std::string`
  em headers por serviço (`src/service_broker/schemas/{farm,field,device,sensor,
  device_config}_schemas.h`), seguindo o mesmo padrão já usado em `schemas/service.h`.
  `device_config.update` só exige a presença de `config` (shape interna arbitrária,
  serializada como está pelo `DeviceConfigService`).
- ✅ (2026-07-14) `SchemaValidator` conectado no `HttpGateway`, não no
  `dispatchCapability`: novo `CapabilitySchemaRegistry`
  (`src/service_broker/schemas/capability_schema_registry.{h,cpp}`) monta um mapa
  capability → `SchemaValidator` na inicialização; `HttpGateway::registerRoutes()`
  valida o body contra esse registro logo após o check de JSON sintaticamente válido
  já existente e antes de `gateway_.sendRequest(...)` — capability sem schema
  registrado (reads, deletes) simplesmente não é validada.
- ✅ (2026-07-14) Testes em `src/service_broker/tests/test_http_e2e.cpp`: payload sem
  campo obrigatório (`farm.create` sem `name`) → 400 com `details` citando o campo, e
  confirma que o backend mockado **não** é chamado; `device_config.update` sem
  `config` → 400; payload válido de `farm.create` segue validação e chega no backend
  normalmente (regressão). Suite completa (`service_gateway_http_e2e_test`, 11 casos)
  e `shared_validation_tests`/`service_gateway_test` passando.
- ✅ (2026-07-15) Achado durante debug manual: `HttpGateway` tem **duas** rotas de
  entrada pras mesmas capabilities — o handler `POST /invoke/:capability` (onde a
  validação acima foi plugada) e um `restHandler` catch-all separado (`server_.Put`/
  `Patch`/`Delete`/`Get`/`Post` em rotas REST-style tipo `PUT /devices/:id`, que resolve
  a capability via `EventRouter.resolveFromPath`). O `restHandler` despachava direto
  pro bus sem passar pelo `CapabilitySchemaRegistry` — ex.: `PUT /devices/123` pra
  `device.update` não era validado, só `POST /invoke/device.update` era. Corrigido
  aplicando a mesma checagem (`schemaRegistry_.validate(capability, bodyCheck)`) dentro
  do `restHandler`, antes do `gateway_.sendRequest(...)` — ambos os caminhos de entrada
  agora validam contra o schema. Suite `service_gateway_http_e2e_test` (11 casos)
  seguiu passando após a mudança.
- Critério de aceite: POST/PUT com campo obrigatório faltando ou tipo errado nunca chega
  no serviço de domínio — 400 direto do gateway. ✅ validado via teste e2e
  (`SchemaValidation_MissingRequiredField_Returns400` confirma `backendCalled == false`),
  cobrindo a rota `/invoke/:capability`; a rota REST-style (`PUT /devices/:id` etc.)
  reaproveita o mesmo registro de validação (ver nota acima), sem teste e2e dedicado
  ainda — considerar adicionar um caso via `restHandler` numa próxima passada.

---

### Backlog — `identity.environment` hardcoded como `"prod"` em todos os serviços

**Contexto (2026-07-09):** todo `App*Service.cpp` seta `identity.environment = "prod"` na
construção, não importa se está rodando em dev, QA ou prod de verdade — apesar do
`Config::getEnvironment()` (`src/shared/config/config.cpp`) já ler `RDWS_ENVIRONMENT` do
ambiente pra exatamente esse propósito.

**Corrigido no `device_service`** (prova de conceito): `identity.environment =
rdws::Config().getEnvironment();` — replicar o mesmo padrão nos outros 7
(`auth_service`, `farm_service`, `field_service`, `device_config_service`,
`sensor_service`, `sensor_reading_service`, `persistence_service`) conforme forem sendo
revisados, um de cada vez (mesmo ritmo do resto do CRUD hardening desta sessão).

- ✅ (2026-07-13) Fix replicado nos 7 serviços restantes — todo `App*Service.cpp` real
  agora usa `identity.environment = rdws::Config().getEnvironment();`. Único lugar que
  ainda hardcoda é `example_service.cpp` (`devMode ? "dev" : "prod"`), mantido assim de
  propósito por ser serviço de demo/exemplo, fora da suite real.
- ✅ (2026-07-13) `RDWS_ENVIRONMENT: qa` adicionado ao bloco `x-db-env` de
  `docker-compose.qa-app.yml`, propagado pra todos os `App*Service` que herdam `*db-env`
  (default do `Config::getEnvironment()` é `"test"` quando `RDWS_ENVIRONMENT` não está
  definido; esta nota foi ajustada para refletir o comportamento real em
  `src/shared/config/config.cpp`).
- ✅ Critério de aceite atendido: `GET /status`/`GET /connections` do gateway devem
  mostrar `environment: "qa"` pros serviços rodando em QA a partir do próximo redeploy
  de `docker-compose.qa-app.yml`.

---

### Backlog — catch genérico vazando detalhes internos (`e.what()`) pro cliente

**Contexto (2026-07-10, achado em code review):** `PostgreSQLDatabase::execQuery`/
`execCommand` fazem `throw std::runtime_error(...)` em erro de SQL/conexão
(`src/shared/database/postgresql_database.cpp`). Como nenhuma camada de serviço captura
essas exceções, elas sobem até o catch genérico de `processRequest` em cada
`App*Service.cpp`, que devolvia `"Internal error: " + e.what()` — vazando texto do driver
Postgres (nomes de coluna, fragmento de query, etc.) direto na resposta HTTP pro cliente.

**Corrigido no `device_service`** (prova de conceito): o catch genérico continua logando
`e.what()` completo no servidor, mas devolve pro cliente apenas `"Internal server error"`
com status 500 — sem detalhes internos. Decisão: resolver no ponto único do catch genérico
por serviço, não com try/catch espalhado em cada método de `*Service` (menos duplicação,
mesmo raciocínio do fix de `identity.environment` acima).

- ✅ Replicado o mesmo fix nos outros 7 serviços (`auth_service`, `farm_service`,
  `field_service`, `device_config_service`, `sensor_service`, `sensor_reading_service`,
  `persistence_service`) — catch genérico de `processRequest` em cada um devolve
  `"Internal server error"` (500) pro cliente, mantendo `e.what()` só no log do servidor.
  Build completo validado (`cmake --build`, todos os 8 serviços linkam sem erro).

---

### Backlog — auditoria (`updated_by`/`updated_at`) e localização de device via GPS

**Contexto (2026-07-10):** ao estender `device.create` para aceitar `installation_date`
opcional (`DeviceCreate::installationDate`, `src/shared/repository/DeviceRepository.h/.cpp`,
`src/services/device_service/AppDeviceService.cpp::handleCreate`), duas questões relacionadas
ficaram de fora do escopo por decisão do usuário:

- **`location` do device não vem no payload de create/update.** A localização de um device é
  adquirida via GPS e chega através dos frames de leitura do sensor — a tabela `devices` deve
  ser atualizada por esse fluxo (ainda não implementado), não por um campo `location` na
  requisição de create como acontece em `farms`/`fields` (`FarmRepository`/`FieldRepository`,
  que recebem `{lat, lng}` e convertem para WKT). Repositório de `Device` permanece sem
  suporte a escrita de `location`.
- **`updated_by`/`updated_at` não são setados em nenhum serviço hoje** (`farms`, `fields`,
  `sensors`, `devices`, `device_config` — todos têm as colunas no schema e leem no SELECT, mas
  nenhum `INSERT`/`UPDATE` as popula). Não há hoje nenhuma propagação da identidade do
  chamador (claims do JWT) até a camada de repositório. Levantado como possível "sistema de
  auditoria" completo (quem criou/alterou cada registro), não apenas os campos soltos.

- ✅ Definir e implementar propagação de identidade do chamador (JWT `sub`/claims) desde o
  `HttpGateway`/middleware de auth até os handlers de cada serviço, para popular `updated_by`
  de forma consistente em todos os CRUDs.
- ✅ Desenhar fluxo de atualização de `devices.location` a partir de leituras de GPS recebidas
  via `sensor_reading_service` (ou capability dedicada), incluindo se `location` deve refletir
  a última leitura ou ter histórico próprio. Decisão: `devices.location` continua sendo
  sobrescrito a cada leitura (reflete a posição atual); histórico fica em nova tabela
  `device_location_history`, populada por trigger em `devices` (`AFTER UPDATE OF location`)
  que só grava uma nova linha quando o deslocamento ultrapassa um limiar configurável por
  device (`device_configurations.config->>'location_threshold_m'`, fallback 10m), para
  filtrar ruído de imprecisão de GPS. Ver `db/migrations/V4__device_location_history.sql`.
- ⬜ Avaliar se isso deve virar um sistema de auditoria mais amplo (ex. tabela de audit log com
  quem/quando/o quê mudou) em vez de apenas preencher `updated_by`/`updated_at` inline.

**Metadados livres do device (fabricante, número de série, modelo, etc.) — 2026-07-10:**
levantada a necessidade de guardar informações do fabricante do device (fabricante, modelo,
número de série, e o que mais surgir por tipo de device). Ideia: seguir o mesmo padrão já
adotado em `device_config` — uma coluna `JSONB` (ex. `devices.metadata`, default `'{}'`) em
vez de colunas fixas, já que o conjunto de atributos varia por fabricante/tipo de sensor e
cresceria via migração toda hora se fossem colunas rígidas. Endpoint de update seguiria o
mesmo merge-patch (`mergePatch`, `src/shared/utils/json_merge.h`) já usado por
`device_config.update`, em vez de reescrever o objeto inteiro a cada PATCH.

- ⬜ Adicionar coluna `devices.metadata JSONB NOT NULL DEFAULT '{}'` via migration Flyway.
- ⬜ `device.create` aceita `metadata` opcional (objeto JSON livre); `device.update` (ou nova
  capability `device.update-metadata`) faz merge-patch igual ao `device_config`.
- ⬜ Decidir se `metadata` é exposto no mesmo endpoint de `device` ou se, a exemplo de
  `device_config`, vale a pena separar em recurso próprio 1:1 (mesmo trade-off já discutido
  para `device_config`: tudo num objeto só x endpoint dedicado).
- ⬜ Suportar busca/filtro por campo dentro do `metadata` (ex. `device.list` com
  `?manufacturer=X`) — JSONB permite `metadata->>'manufacturer' = $1` direto; para volume
  maior, criar índice de expressão `CREATE INDEX ON devices ((metadata->>'manufacturer'))`
  para os campos mais buscados, ou um índice `GIN` genérico (`USING GIN (metadata)`) com
  operador `@>` se a busca precisar cobrir múltiplos campos sem saber quais de antemão.

---

### Backlog — wrappers semânticos para `ResponseHelper::returnErrorDoc`

**Contexto (2026-07-11):** ao corrigir os retornos 500 indevidos em validações de CRUD
(`ResponseHelper::returnErrorDoc`/`returnError` — o parâmetro `statusCode` deixou de ter
default e agora é obrigatório em `returnErrorDoc`, forçando todo call site a ser explícito),
ficou claro que os handlers ainda escrevem o código HTTP na mão em cada chamada
(`returnErrorDoc("Device not found", 404)`, `returnErrorDoc("Missing field: type", 400)`,
etc.) — fácil de errar o número ou ser inconsistente entre serviços.

**Ideia:** adicionar wrappers semânticos em `ResponseHelper` (`src/shared/utils/response_helper.h/.cpp`)
que fixam o status code pelo nome do método, ex.:
- `returnErrorNotFound(message)` → 404
- `returnErrorBadRequest(message)` → 400 (o caso mais comum: campo/parâmetro inválido ou faltando)
- `returnErrorInternalError(message)` → 500
- `returnErrorUnauthorized(message)` / `returnErrorForbidden(message)` → 401/403

- ⬜ Adicionar os wrappers em `ResponseHelper`.
- ⬜ Migrar os `App*Service.cpp` (todos os 8 serviços) pra usar os wrappers em vez de
  `returnErrorDoc(msg, <código na mão>)`.
- ⬜ Critério de aceite: nenhum código HTTP mágico espalhado pelos handlers — só nos
  wrappers do `ResponseHelper`.

---

### Fase 14 - Escalabilidade horizontal do Gateway (baixa prioridade, backlog)

**Contexto:** o `HttpGateway`/`ServiceGateway` hoje assume instância única. Levantamento feito em 2026-07-06 identificou os seguintes acoplamentos de estado local que impedem rodar múltiplas réplicas atrás de um load balancer:

- Backends (ex. Sensor Simulator) se conectam via socket TCP/Unix a **uma instância específica** do gateway; `ServiceRegistry`/`activeConnections`/`pendingRequests` vivem só na memória do processo (`Services/ServiceGateway.h:66-73`). Não há discovery compartilhado entre instâncias — uma request roteada para a instância errada falha.
- Unix socket em path fixo `/tmp/service_gateway.sock` (`Services/ServiceGateway.h:53`) colide entre instâncias no mesmo host/container.
- `GatewayConfig` (capabilities/feature flags) e `EventRouter` (regras de roteamento) persistem em **arquivo JSON local** — mudanças via `PATCH /config/...` numa instância não propagam para as outras.
- `EventBus` interno é só em memória por processo — métricas/eventos (`request.completed`, `metrics.snapshot`) refletem apenas o que aquela instância processou, sem agregação entre réplicas.

**Decisão:** não priorizar agora. Finalizar as implementações e o deployment single-instance atuais antes de investir em multi-réplica. Este item fica no final do backlog geral (depois de `Plano_Deployment.md` e `Plano_SensorSimulatorService.md`).

**Exceção já decidida:** mover `routes.json` (EventRouter) e demais configurações hoje persistidas em arquivo local (`GatewayConfig`) para o banco de dados, em vez de arquivo — isso pode ser feito no mesmo esforço da Fase 10a (PersistenceService) ou como preparação leve para esta fase, sem exigir o resto do trabalho de multi-instância.

- ⬜ Migrar `GatewayConfig` (capabilities/features) de arquivo JSON para tabela no banco via `PersistenceService`.
- ⬜ Migrar `EventRouter` (`routes.json`) de arquivo para tabela no banco.
- ⬜ (Backlog, sem data) Discovery compartilhado entre instâncias do gateway (ex. Redis) para registro de serviços conectados.
- ⬜ (Backlog, sem data) Agregação de métricas entre instâncias (fora do processo, via `PersistenceService`).
- ⬜ (Backlog, sem data) Definir estratégia de roteamento cross-instância vs. afinidade backend→instância.
- Critério de aceite (só da parte de config em banco): `PATCH /config/capabilities/{cap}` e alterações de rotas persistem no banco e sobrevivem a restart sem depender de arquivo local.

---

## Próximo passo sugerido
Fases 9b (AuthService) e 10a (PersistenceService) já implementadas e testadas. Próximo
passo: **Fase 10b (CI/CD + Docker)** e **Fase 11 (Loki + Grafana)**, detalhadas em
`Plano_Deployment.md`. `Plano_Ingestion.md` (RabbitMQ) entra depois, reaproveitando o
mesmo pipeline de CI/CD e a mesma instância de Grafana. O `Plano_SensorSimulatorService.md`
(ferramenta de apoio para o pipeline de ingestão) só é executado depois — por último na
ordem de implementação do `Plano_Deployment.md` (§6, passo 8), após tudo dockerizado e
rodando em QA e prod.
