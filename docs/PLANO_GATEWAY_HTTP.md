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

### Fase 10 - DB/Flyway e CI/CD
- Persistência de request history e métricas em banco de dados.
- Pipeline de CI/CD com build, testes e deploy automatizados.

---

## Próximo passo sugerido
Concluir a **Fase 5** com testes HTTP end-to-end: levantar um service mock via socket, disparar requests HTTP reais ao gateway e validar respostas — cobrindo cenários de sucesso, timeout e auth rejeitada. Isso fecha o único gap de cobertura antes de avançar para Fase 10.
