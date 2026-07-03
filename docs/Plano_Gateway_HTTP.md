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
- ⬜ API HTTP para leitura/atualização de config em runtime.

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

- ⬜ Criar tabela `users` via migration Flyway (ver `Plano_DB_IOT_Sensors.md`).
- ⬜ Implementar `AuthService` com validação de credenciais e emissão de JWT HS256.
- ⬜ Registrar `/auth/login` como caminho público no gateway.
- ⬜ Adicionar capability `auth.login` no EventRouter.
- Critério de aceite: POST /auth/login com credenciais válidas retorna JWT aceito pelo gateway nas requisições seguintes; credenciais inválidas retornam 401.

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

- ⬜ Definir e publicar eventos `request.completed` e `metrics.snapshot` no EventBus do gateway.
- ⬜ Implementar `PersistenceService` com buffer interno e batch upsert no PostgreSQL.
- ⬜ Criar migrations Flyway para `request_history` e `capability_metrics`.
- ⬜ Implementar job de cleanup de registros antigos.
- ⬜ Adicionar `PersistenceService` como datasource no Grafana (fase 11).
- Critério de aceite: queda do PersistenceService não afeta gateway; request history e métricas consultáveis no banco após reconexão.

---

#### 10b — CI/CD (GitHub Actions + self-hosted runner + Docker)

**Decisão de design:** GitHub Actions com self-hosted runner no servidor Fedora doméstico. Build e testes rodam dentro de container Docker para ambiente reproduzível (GCC, CMake, dependências). Deploy é substituição do container em execução no mesmo servidor.

**Pipeline:**
```
push/PR → build Docker → testes unitários + e2e → (merge main) → deploy container
```

- ⬜ Criar `Dockerfile` de build multi-stage (builder com GCC/CMake → imagem final mínima).
- ⬜ Instalar e registrar self-hosted runner no servidor Fedora.
- ⬜ Criar workflow `ci.yml`: build + testes em todo PR.
- ⬜ Criar workflow `deploy.yml`: triggered em merge na main; para container anterior, sobe novo.
- ⬜ Configurar secrets no GitHub (credenciais do banco, API keys de teste).
- Critério de aceite: PR abre → CI roda automaticamente; merge na main → novo container em produção sem intervenção manual.

### Fase 11 - Observabilidade de Logs (Loki + Grafana)

**Decisão de design:** logs não vão para o banco. O gateway já emite logs estruturados JSON via spdlog com arquivo rotativo. O Promtail lê esse arquivo e envia para o Loki; o Grafana consome ambos (PostgreSQL para métricas/requests, Loki para logs).

- ⬜ Configurar **Loki** como datasource de logs (retenção configurável, ex: 30 dias).
- ⬜ Configurar **Promtail** apontando para o log file rotativo do gateway (zero mudança no código C++).
- ⬜ Adicionar datasource Loki no Grafana.
- ⬜ Criar dashboard no Grafana unificando:
    - Métricas por capability (latência, errorRate, throughput) — fonte: PostgreSQL.
    - Exploração de logs por `requestId`, `capability`, `level` — fonte: Loki via LogQL.
- ⬜ Definir política de retenção no Loki (período e compressão).
- Critério de aceite: dado um `requestId`, conseguir navegar do painel de métricas até os logs daquele request no Grafana.

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

## Próximo passo sugerido
Concluir a **Fase 5** com testes HTTP end-to-end: levantar um service mock via socket, disparar requests HTTP reais ao gateway e validar respostas — cobrindo cenários de sucesso, timeout e auth rejeitada. Isso fecha o único gap de cobertura antes de avançar para Fase 10.
