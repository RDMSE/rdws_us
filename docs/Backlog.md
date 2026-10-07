# Backlog — rdws_us (backend)

Pendências consolidadas dos planos deste repositório, em 2026-10-07. Cada item aponta o
plano de origem, onde estão o contexto e as decisões. Ao concluir um item, marque-o no plano
de origem e remova-o daqui. O backlog do firmware fica em
`rdws_weather_node/docs/Backlog.md`.

Legenda de prioridade: **P1** = risco ou bloqueio real hoje; **P2** = próximo passo natural;
**P3** = quando a necessidade aparecer.

## Telemetria e ingestão

Origem: `docs/Plano_Telemetria.md` e `docs/Plano_Ingestion.md`.

| Prio | Item | Observação |
|---|---|---|
| P1 | Canal de notificação dos alertas de frota | O alerta "Silent station" só aparece no Grafana (R2) |
| P2 | Faixas plausíveis no `IngestionService` (ex.: umidade > 100 %) | Dono do sensor e unidade já são conferidos |
| P2 | Retry/DLQ no `ReadingWriterService` para mensagens que falham sempre | Hoje uma falha de banco deixa a mensagem sem ack |
| P2 | Desativar o formato JSON legado no `IngestionService` | Firmware e simulador já usam SenML; falta decidir quando |
| P3 | Formato e tabela das regras de edge trigger (`rul`) | Junto com o passo 8 do firmware |
| P3 | Invalidação de credencial em tempo real | Hoje o cache de PSK é por poll de 60 s |
| P3 | Partições de `sensor_readings`/`device_telemetry` | Adiado (R3): revisitar com ~50 mi de linhas ou dashboard lento |
| P3 | Retenção/compressão de `sensor_readings` | Tabela de retenção do `Plano_DB_IOT_Sensors.md` não implementada |
| P3 | Coluna `category` em `sensors` (saúde vs. ambiental) | Para o dashboard do produtor |
| P3 | Fase 4: SenML CBOR (`zcbor` + parser, Content-Format 112) | Quando o NB-IoT pedir |
| P3 | Estação agregadora (`devices.parent_device_id`, `bn` de nós filhos) | Seção "Futuro" do `Plano_Telemetria.md` |

## Alarmes do produtor

Origem: `docs/Plano_Alerting.md`. Nada implementado ainda; a ingestão (pré-requisito) já está
estável.

| Prio | Item | Observação |
|---|---|---|
| P2 | Migration `alarm_rules` e `alarm_events` | Passo 2 da ordem de implementação |
| P2 | Filtro de primeira ordem (Camada 1) no `ReadingWriterService` | Passo 3 |
| P2 | `AlertingService` (Camada 2: janelas, histerese, manutenção) | Passo 4. Tende a ser worker puro |
| P3 | `NotificationService` (escalonamento, reconhecimento, provedor de SMS) | Passo 5; precisa de plano próprio |
| — | Decisões em aberto: evento `alarm_rule.changed`, transporte de `reading.threshold_breach`, sensor "principal" de métricas derivadas, fonte única dos limites, tendência vs. média, cadeia de contatos por fazenda | Seção "Pontos em aberto" |

## Gateway

Origem: `docs/Plano_Gateway_HTTP.md`.

| Prio | Item | Observação |
|---|---|---|
| P1 | `ServiceClient` não reconecta depois de perder a conexão com o gateway | Visto no QA em 2026-07-22; mitigação manual com `docker restart` |
| P1 | CRUD de `/routes` sem autenticação | 5 handlers em `HttpGateway.cpp` |
| P2 | Fase 13: IDOR / `public_id` nas tabelas expostas | Muda o contrato da API |
| P3 | Wrappers semânticos no `ResponseHelper` | Tirar códigos HTTP soltos dos handlers |
| P3 | Metadados livres do device (`devices.metadata JSONB`) | Fabricante, série, modelo |
| P3 | Testes de carga leve com várias capabilities simultâneas | Fase 5 |
| P3 | Link painel → logs do request no Grafana | Fase 11 |
| P3 | Sync de `routes.json` também no `deploy-prod.yml` | Já existe no QA |
| P3 | Fase 14: multi-instância; `routes.json`/`GatewayConfig` no banco | Por último, sem data |
| P3 | Auditoria mais ampla (tabela de audit log) | Avaliação |

## Infra e deploy

Origem: `docs/Plano_Deployment.md`.

| Prio | Item | Observação |
|---|---|---|
| P2 | Provisionar a VPS e o primeiro deploy de produção | Fora do alcance de automação |
| P2 | Endpoint CoAP público na VPS + relay UDP para o QA (§2.2) | Quando a primeira estação sair da bancada |
| P3 | Secret `GRAFANA_ADMIN_PASSWORD` no environment `qa` e no `.env.qa` gerado | Hoje cai no padrão se o volume do Grafana se perder |
| P3 | Habilitar "Log volume" no Loki (`volume_enabled`) | Backlog |

## Credenciais de device

Origem: `docs/Plano_DeviceCredentials.md`.

| Prio | Item | Observação |
|---|---|---|
| P2 | Grace period na rotação de PSK | Hoje a rotação derruba o device até reprovisionar |
| P3 | Versionamento da KEK | Quando houver necessidade real |
| P3 | TLS/mTLS no canal broker ↔ serviços | Se a topologia sair de um único host |
| P3 | Convergir `psk_identity` com `public_id` (Fase 13) | Avaliar junto com a Fase 13 |

## Banco e simulador

| Prio | Item | Origem |
|---|---|---|
| — | Tipos novos em `sensor_type` (bateria, solar, vento, chuva, radiação); avaliar `co2` → `eco2` | Pedido pelo firmware (passos 5–6, ver o backlog dele) |
| P3 | Simulador gerando a janela OMM (vento) | `Plano_SensorSimulatorService.md` |
| P3 | Cenários de falha propositais no simulador (fora da faixa, perda, offline) | `Plano_SensorSimulatorService.md` |
