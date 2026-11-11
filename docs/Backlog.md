# Backlog — rdws_us (backend)

Pendências consolidadas dos planos deste repositório, em 2026-10-07 (revisado em 2026-10-09,
2026-10-28, 2026-11-05 e 2026-11-10). Cada item aponta o
plano de origem, onde estão o contexto e as decisões. Ao concluir um item, marque-o no plano
de origem e remova-o daqui. O backlog do firmware fica em
`rdws_weather_node/docs/Backlog.md`.

Legenda de prioridade: **P1** = risco ou bloqueio real hoje; **P2** = próximo passo natural;
**P3** = quando a necessidade aparecer.

## Telemetria e ingestão

Origem: `docs/Plano_Telemetria.md` e `docs/Plano_Ingestion.md`.

| Prio  | Item | Observação |
|-------|------|---|
| P2    | Canal de notificação dos alertas de frota | O alerta "Silent station" só aparece no Grafana (R2). Adiado em 2026-10-23: fazer quando a primeira estação sair da bancada (escolher Telegram, ntfy ou e-mail) |
| P3    | Resolver `crash_pc` em arquivo e linha no servidor | Hoje é manual, com `addr2line` e o `.elf` da máquina do desenvolvedor. Depende da release por tag do firmware, que leva o `.elf` ao servidor |
| P3    | Retenção das falhas | `crash_*` somem com a limpeza de 90 dias de `device_telemetry`; são raras e servem para comparar versões (`Plano_Telemetria.md`, pontos em aberto) |
| P3    | Não reenviar a config que o device recusou | Hoje, com `cfg_rej` = versão atual, a ingestion reenvia a mesma config no `2.04` de todo ciclo, e a estação recusa de novo. Inofensivo, mas gasta rádio; dá para pular o corpo quando `cfg_rej` = `version` |
| P3    | Tabela `device_packets` para o `seq` (detecção de perda) | Decidido em 2026-11-09. Hoje o `seq` vai para `device_telemetry` com o tempo da primeira leitura do pacote (`bt`), numa linha separada da do diagnóstico, que usa o tempo do envio. O upsert por (`device_id`, `timestamp`) faz um reenvio (mesmo `bt`, `seq` novo) sobrescrever o `seq` anterior, e cada lote vira uma linha só com `{"seq": N}`. Proposta: uma linha por pacote, (`device_id`, `boot_count`, `seq`, `received_at`, `bt`, registros), chave (`device_id`, `boot_count`, `seq`); `seq` sai da `device_telemetry`. Colunas `BIGINT`: `seq` e `boot_count` são `uint32` no firmware (o `seq` volta a 0 no estouro e recomeça a cada boot). Comparar `seq` em módulo 2³² (RFC 1982). Falta decidir como cada pacote leva o `boot_count`: a ingestion copia o do diagnóstico do ciclo, ou o firmware põe o `boot_count` em todo pacote (~25 bytes; preferido, sem estado na ingestion). A migração limpa o `seq` da `device_telemetry` tirando a chave, não a linha, porque o upsert juntou `seq` e diagnóstico quando os instantes coincidiram (pacote de crash, dois pacotes no mesmo segundo): `UPDATE ... SET data = data - 'seq' WHERE data ? 'seq'` e depois `DELETE ... WHERE data = '{}'`. Sem backfill da `device_packets`: as linhas antigas não têm `boot_count`, e os reenvios já sobrescreveram parte dos `seq` |
| P3    | Contar duplicatas descartadas pelo `ON CONFLICT` no `ReadingWriterService` | Somem sem log nem métrica; teriam mostrado a regressão do RDWS-157 no mesmo dia |
| P3    | Insert em lote no `ReadingWriterService` (uma transação por N mensagens) | Hoje ~16 ms por leitura (~62 msg/s), já acompanha o upload; só se o volume crescer muito |
| P3    | Uma conexão AMQP com os dois consumers no `ReadingWriterService` | Tira a espera de até 500 ms da telemetria com as filas vazias |
| P3    | `extra_uplinks_utc` no schema + políticas de agendamento no `DeviceConfigService` (`sunset_frost`) | §3 do plano de firmware. Precisa de agendador diário e das datas do período de geada |
| P3    | Invalidação de credencial em tempo real | Hoje o cache de PSK é por poll de 60 s |
| P3    | Partições de `sensor_readings`/`device_telemetry` | Adiado (R3): revisitar com ~50 mi de linhas ou dashboard lento |
| P3    | Retenção/compressão de `sensor_readings` | Tabela de retenção do `Plano_DB_IOT_Sensors.md` não implementada |
| P3    | Coluna `category` em `sensors` (saúde vs. ambiental) | Para o dashboard do produtor |
| P3    | Fase 4: SenML CBOR (`zcbor` + parser, Content-Format 112) | Quando o NB-IoT pedir |
| P3    | Estação agregadora (`devices.parent_device_id`, `bn` de nós filhos) | Seção "Futuro" do `Plano_Telemetria.md` |

## Alarmes do produtor

Origem: `docs/Plano_Alerting.md`. Nada implementado ainda; a ingestão (pré-requisito) já está
estável.

| Prio | Item | Observação |
|---|---|---|
| P2 | Migration `alarm_rules` e `alarm_events` | Passo 2 da ordem de implementação |
| P2 | Filtro de primeira ordem (Camada 1) no `ReadingWriterService` | Passo 3 |
| P2 | `AlertingService` (Camada 2: janelas, histerese, manutenção) | Passo 4. Tende a ser worker puro |
| P3 | Alertas sobre índices derivados: `rule_type` + `params JSONB`, métricas novas (`pressure`, `vpd`, `delta_t`, `pressure_tendency_3h`), avaliação por (device, timestamp), notificação de leituras atrasadas | Passo 5; depois das views e de `daily_weather` (seção "Índices derivados" abaixo) |
| P3 | `NotificationService` (escalonamento, reconhecimento, provedor de SMS) | Passo 6; precisa de plano próprio |
| — | Decisões em aberto: evento `alarm_rule.changed`, transporte de `reading.threshold_breach`, fonte única dos limites, tendência vs. média, cadeia de contatos por fazenda, schema de `params` por tipo, parâmetros de geada/doença (agrônomo), limite de idade para notificar, estágios de trigger para geada; confirmar o sensor da grandeza dominante em `alarm_events.sensor_id` | Seção "Pontos em aberto" |

## Índices derivados

Origem: `docs/Plano_Indices_Derivados.md`. Nada implementado ainda; segue a ordem de
implementação do plano.

| Prio | Item | Observação |
|---|---|---|
| P2 | Funções SQL puras (`es`, `dewpoint`, `vpd`, `wet_bulb`, `slp`, `ra`, `et0_hs`, `et0_pm`) com testes de referência | Passo 2. FAO-56 e Stull 2011 |
| P2 | View `weather_samples` (com `window_s`) e views instantâneas (Td, DPV, Delta T, P0, tendência 3 h) | Passo 3 |
| P2 | Painéis instantâneos no Grafana | Passo 4 |
| P2 | `daily_weather` + fila `dirty_days` + job de recálculo | Passo 5. Decidir antes onde o job roda (`pg_cron` ou worker) |
| P3 | `wetness_periods` | Passo 6 |
| P3 | `crops`, `field_seasons` e acumulados (GDD, frio Weinberger) | Passo 7 |
| P3 | Painéis diários no Grafana | Passo 8 |
| P3 | Altitude automática por modelo digital de elevação | D7. Decidir raster no PostGIS ou API externa |
| — | Decisões em aberto: sensor canônico por grandeza, ponto de geada sobre gelo, cobertura mínima do dia, agregação horária (Utah/Dynamic), talhão com várias estações, `kRs`/vento default, faixas agronômicas | Seção "Pontos em aberto" |

## Gateway

Origem: `docs/Plano_Gateway_HTTP.md`.

| Prio | Item | Observação |
|---|---|---|
| P2 | Fase 13: IDOR / `public_id` nas tabelas expostas | Muda o contrato da API |
| P3 | Posição (`reportLocation`) fora do thread CoAP | Hoje é um `invoke` síncrono de até 5 s por pack com `lat`/`lon`, no único thread do ingestion: a mesma classe de risco da regressão do RDWS-157. Mover para mensagem na fila com o writer gravando em `devices.location`, como o `config_span`. Só importa quando uma estação mandar posição |
| P2 | Timeout de PONG no `ServiceClient` (conexão meio-aberta) | A reconexão do RDWS-92 só age quando o `recv()` falha; se o gateway some sem FIN, o cliente fica preso. Mais provável quando os serviços saírem do mesmo host |
| P2 | Trocar a senha do admin no QA (ainda é o `changeme` do seed da V1) e criar um usuário admin próprio pro deploy | O sync de rotas do `deploy-qa.yml` usa os secrets `RDWS_ADMIN_USER`/`RDWS_ADMIN_PASSWORD` (RDWS-155). Junto: `adminPass` como variável secret nos environments do Bruno |
| P3 | Wrappers semânticos no `ResponseHelper` | Tirar códigos HTTP soltos dos handlers |
| P3 | Metadados livres do device (`devices.metadata JSONB`) | Fabricante, série, modelo |
| P3 | Testes de carga leve com várias capabilities simultâneas | Fase 5 |
| P3 | Link painel → logs do request no Grafana | Fase 11 |
| P3 | Sync de `routes.json` também no `deploy-prod.yml` | Já existe no QA |
| P3 | Rotas (`EventRouter`) e `GatewayConfig` no banco | "Exceção já decidida" da Fase 14, sem esperar a multi-instância. Rotas do repo entram por migration Flyway (remover uma rota = migration com `DELETE`); coluna `source` (`repo`/`runtime`) separa as criadas via `POST /routes`. Substitui o `sync_gateway_routes.py` e a semeadura do `docker-entrypoint.sh`. Até lá, o QA mantém `POST`/`PUT`/`DELETE /devices/{id}/config` (removidas no RDWS-16) — apagar à mão com token admin |
| P3 | Fase 14: multi-instância (discovery compartilhado, métricas agregadas) | Por último, sem data. A parte de config no banco está no item acima |
| P3 | Auditoria mais ampla (tabela de audit log) | Avaliação |
| P3 | Testes do gateway instáveis em `ctest -j` | Cada caso é um processo (`gtest_discover_tests`) e vários usam portas e sockets fixos, então colidem em paralelo (`ServiceReconnects_SameId`, `Invoke_CallerReachesResponder`). Em série passam. Dar uma porta por teste, como no `test_coap_dtls_client.cpp`, ou `RUN_SERIAL`. Parte das falhas era a corrida do RDWS-184 (resposta antes do `waitForResponse`, corrigida); em 2026-11-11 ainda falhou 1 de 3 rodadas com `-j4` (`Invoke_CallerReachesResponder`), então as portas fixas continuam |

## Infra e deploy

Origem: `docs/Plano_Deployment.md`.

| Prio | Item | Observação |
|---|---|---|
| P2 | Provisionar a VPS e o primeiro deploy de produção | Fora do alcance de automação |
| P2 | Endpoint CoAP público na VPS + relay UDP para o QA (§2.2) | Quando a primeira estação sair da bancada |
| P2 | Tempo de CI/deploy | Cada merge compila tudo três vezes: o CI (estágio `builder` com testes), o `push-ghcr` (9 builds em série) e o deploy de QA (`docker compose up --build`, mais o `qa_simulators.sh`). O `COPY . .` antes do `cmake` invalida o build inteiro a cada commit, até um só de docs, e as dependências do `FetchContent` são baixadas e compiladas de novo. Opções, da que mais rende: (1) o deploy usa as imagens `:qa` do GHCR com `docker compose pull`, como o de produção, e o `push-ghcr` passa a incluir ingestion, reading_writer e simulador; (2) `ccache` com cache mount do BuildKit; (3) dependências numa camada própria antes de copiar o código; (4) `docs/`, `bruno/` e `infra/` no `.dockerignore`. Medir antes a duração de cada passo nos runs do Actions |
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
