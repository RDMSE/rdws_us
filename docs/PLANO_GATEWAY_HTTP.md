# Plano de evolução do Gateway HTTP (Service Broker)

Data: 2026-05-15

## Objetivo
Consolidar o gateway HTTP em C++ que recebe requisições, transforma em payload de evento Lambda e roteia para o microserviço capaz de atender a capability solicitada.

## Diagnóstico atual do código

### Fluxo já implementado
1. Entrada HTTP no endpoint POST /invoke/{capability}.
2. Conversão da requisição para payload com:
- lambdaEvent (estrutura derivada de LambdaEvent)
- lambdaContext (estrutura derivada de LambdaContext)
- metadados HTTP (headers, queryStringParameters, pathParameters, body, capability)
3. Roteamento no broker via selectBestService(capability).
4. Envio para microserviço por socket (tcp/unix) com mensagem REQUEST.
5. Resposta HTTP imediata 202 (accepted/queued) com requestId.

### Endpoints de observabilidade já implementados
- GET /status
- GET /connections

### Lacunas principais
1. Não há correlação completa de resposta para o cliente HTTP:
- RESPONSE do serviço só é logado no broker.
- Falta armazenar pendingRequests de forma útil e retornar resultado final.

2. Segurança e robustez de protocolo:
- Mensagens por socket sem framing explícito (risco com mensagens parciais/múltiplas em um recv).
- Falta timeout por request com tratamento de expiração.

3. Confiabilidade operacional:
- Cleanup de pendingRequests está TODO.
- Sem retry/failover por capability quando o destino falha após seleção.

4. Testes insuficientes para o fluxo HTTP -> broker -> service:
- Há testes de schema, mas faltam testes de integração de roteamento e ciclo de resposta.

5. Concorrência no registro de serviços:
- Atenção para lock reentrante em operações de remoção/atualização no registry durante manutenção.

## Direção arquitetural recomendada

### Contrato canônico de evento
Padronizar payload enviado ao microserviço com envelope único:
- requestId
- capability
- lambdaEvent
- lambdaContext
- metadata (opcional)

Evitar duplicidade de campos equivalentes em níveis diferentes do JSON.

### Modelo de resposta
Definir um dos dois modos (ou ambos):
1. Async confirmado:
- HTTP retorna 202 + requestId.
- Novo endpoint GET /requests/{requestId} para consultar status/resultado.

2. Sync opcional com timeout curto:
- Header query ou flag waitForResponse=true.
- Gateway aguarda até N ms por RESPONSE e retorna 200/4xx/5xx.
- Se estourar timeout, retornar 202 com requestId.

## Histórico de execução (fases de fundação)

Estas fases foram planejadas e executadas antes da versão atual deste documento.

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
- Implementar armazenamento real de pendingRequests com timestamp e estado.
- Implementar cleanupExpiredRequests.
- Critério de aceite: sem perda de mensagens em testes com payloads maiores e bursts.

### Fase 2 - Ciclo de resposta completo ✅
- Correlacionar RESPONSE ao requestId.
- Persistir resultado em estrutura de request tracking.
- Criar endpoint GET /requests/{requestId}.
- Critério de aceite: requisição HTTP consegue recuperar resultado processado.

### Fase 3 - Modo síncrono opcional ✅
- Adicionar opção waitForResponse com timeout configurável.
- Retornar resposta do microserviço diretamente quando disponível.
- Critério de aceite: smoke test cobrindo timeout, sucesso e erro do serviço.

### Fase 4 - Observabilidade e operação ✅ CONCLUÍDA
- ✅ Métricas por capability: latência (avg, p99, min, max), taxa de erro, contagem de timeouts — `MetricsTracker` com ring buffer 200 amostras.
- ✅ Logs estruturados JSON (spdlog) com requestId ponta a ponta: `http_request`, `http_response`, `response_correlated`, `service_connected`, `service_disconnected`.
- ✅ Endpoint `GET /metrics` — per-capability `requestCount`, `avgLatencyMs`, `p99LatencyMs`, `errorRate`.
- ✅ Endpoint `GET /health` — `status`, `uptimeEpochSec`, gateway stats (`activeConnections`, `pendingRequests`), per-service summary.
- ✅ Log file rotativo opcional via 4º argumento CLI (`logFile`): `./service_gateway_http <brokerPort> <httpPort> <unixSocket> [logFile]`.
- Critério de aceite: troubleshooting por requestId sem inspeção manual de socket. ✅

### Fase 5 - Testes e hardening
- Testes de integração HTTP->broker->service.
- Testes de carga leve com múltiplas capabilities.
- Testes de falha: desconexão, timeout, serviço indisponível.
- Critério de aceite: suíte cobrindo cenários críticos de regressão.

### Fase 6 - EventRouter
- Roteamento dinâmico de eventos entre capabilities e serviços.
- Suporte a regras de roteamento configuráveis.

### Fase 7 - EventBus
- Canal pub/sub interno para desacoplar produtores e consumidores de eventos.
- Integração com o ciclo de vida do ServiceGateway.

### Fase 8 - Auth
- Autenticação e autorização nas rotas HTTP (JWT ou API key).
- Propagação de identidade até o serviço via LambdaContext.

### Fase 9 - Config & feature flags
- Configuração dinâmica de timeout, load balancing e limites por capability.
- Suporte a feature flags por ambiente.

### Fase 10 - DB/Flyway e CI/CD
- Persistência de request history e métricas.
- Pipeline de CI/CD com build, testes e deploy automatizados.

## Backlog técnico imediato
1. Implementar mapa de requests pendentes com estado: queued, in_flight, completed, failed, timeout.
2. Implementar handleResponseMessage com atualização de estado e armazenamento de payload.
3. Criar endpoint GET /requests/{requestId}.
4. Definir timeout padrão de request (ex.: 5s para sync, 30s para async tracking).
5. Adicionar testes de integração mínimos para capability ping/echo.

## Decisões abertas
1. O gateway deve priorizar modelo async puro ou híbrido async+sync?
2. Resultado de request expira em quanto tempo para consulta posterior?
3. Em caso de múltiplos serviços com mesma capability, manter LEAST_LOADED como padrão?

## Próximo passo sugerido
Iniciar a Fase 5 (Testes e hardening): testes de integração HTTP→gateway→service, cenários de falha (timeout, desconexão, serviço indisponível) e suíte de regressão cobrindo capabilities ping/echo.
