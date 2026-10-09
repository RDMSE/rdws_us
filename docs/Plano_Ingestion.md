Data: 2026-07-01

# Plano de Ingestão de Dados de Sensores

O `PLANO_API_REST.md` define o `SensorReadingService` como somente leitura (`sensor_reading.list`, `sensor_reading.get`), mas não descreve como as leituras chegam ao sistema. Este documento cobre o caminho de escrita: como os devices enviam dados até eles ficarem persistidos no banco.

---

## Visão geral do fluxo

```
[Device] --CoAP (DTLS)--> [IngestionService] --parse/valida--> [Fila persistente] --> [ReadingWriterService] --> [Banco]
```

1. **IngestionService** abre uma porta CoAP e fica disponível para os devices se conectarem.
2. Cada device, em horários agendados, envia suas leituras para essa porta.
3. O IngestionService faz o parse do payload, valida contra a config do device (`device_config`) e publica a mensagem em uma fila durável.
4. Um **ReadingWriterService**, na outra ponta da fila, consome as mensagens e grava as leituras no banco (`sensor_reading`).

Esse desenho desacopla a taxa de chegada dos devices (que pode ser irregular, em rajadas, ou sofrer reconexões) da capacidade de escrita do banco, e dá retry/backpressure sem esforço extra.

---

## IngestionService

- Protocolo: **CoAP**, escolhido por causa do hardware low-power. CoAP roda sobre UDP, sem conexão persistente — o device acorda, envia o datagrama e volta a dormir, economizando rádio/bateria. MQTT (ex. Mosquitto) foi considerado e descartado para a entrada dos dados: exige manter uma conexão TCP com keep-alive aberta, custando mais energia do que o cenário permite.
- Com **DTLS** obrigatório — sem isso qualquer dispositivo na rede pode injetar leituras falsas.
- Autenticação por device: **PSK** (resolvido — `Plano_DeviceCredentials.md`), via `libcoap` com backend OpenSSL. Já implementado do lado cliente (`SensorSimulatorService`, ver abaixo); o lado servidor (`coap_context_set_psk2`/`coap_dtls_spsk_info_t`, carregando credenciais ativas do `device_credential.get_active`) ainda não existe.
- Responsabilidades:
  - Receber o payload CoAP do device.
  - Parsear para o formato interno de leitura.
  - Validar contra `device_config` (schema esperado, ranges plausíveis) — leituras fora do schema são descartadas/logadas, não publicadas na fila.
  - Publicar na fila persistente.
- Não acessa o banco diretamente — é stateless em relação à persistência.
- **Contrato de payload e identidade: ver `Plano_Telemetria.md`.** Migração do JSON atual
  (Content-Format `50`) para SenML JSON (`110`, depois CBOR `112`), com despacho por
  Content-Format durante a transição; device derivado da `psk_identity` do handshake
  (Fase 0, ✅ 2026-10-05); `sensor_id` global no `n`, conferido contra o dono por cache
  (`sensor.list_owners`);
  diagnóstico do device roteado para `device_telemetry`; corpo da resposta CoAP com
  `config_version`/`rules_version` (D11).
- Exposição à internet (devices reais sem Tailscale): VPS como porta de entrada pública,
  com relay UDP para o QA no homelab e hardening da porta — ver
  `Plano_Deployment.md` §2.2. Para NB-IoT, avaliar DTLS Connection ID (RFC 9146) no
  servidor.

## Fila persistente

- Candidatos avaliados:
  - **RabbitMQ** — fila durável, simples de operar, atende bem o volume esperado (poucos sensores por device, leituras agendadas, não streaming contínuo de alta frequência). Candidato natural.
  - **ActiveMQ (Artemis)** — tecnicamente viável, suporta persistência e múltiplos protocolos (MQTT/AMQP/STOMP), mas é mais pesado de operar; faz mais sentido se o resto da stack já usar JMS/Java, o que não é o caso aqui.
  - **ZeroMQ** — descartado: é uma biblioteca de mensageria, não um broker. Não tem persistência em disco, fila durável nem ack/redelivery nativos — exigiria reimplementar tudo isso manualmente, o que vai contra o requisito de persistência da fila.
  - **Mosquitto (MQTT)** — descartado como fila/broker para este ponto: tem persistência limitada (não foi desenhado como fila enterprise durável) e, mesmo se usado, ainda exigiria repassar as mensagens para uma fila persistente de verdade antes do ReadingWriterService.
- Dado o volume esperado, RabbitMQ com fila durável atende sem complexidade extra de operação.
- Mensagem deve carregar: `device_id`, `sensor_id`, `timestamp`, `valor`, `unidade` (ou o que o schema de `sensor_reading` exigir).

## ReadingWriterService

- Consome da fila e grava em `sensor_reading`.
- **Idempotência obrigatória**: se o consumer falhar e a mensagem for reprocessada, a escrita não pode duplicar a leitura. Usar chave única `(sensor_id, timestamp)` no banco — `sensor_readings`
  (`Plano_DB_IOT_Sensors.md`) só tem `sensor_id`, sem `device_id` (o device já é derivado via
  `sensor.device_id`); adicionar `device_id` aqui seria dado redundante, o mesmo princípio que
  levou à remoção de `farm_id` de `devices` no mesmo documento.
- Não expõe endpoints HTTP — é um worker puro, sem capability registrada no gateway.

---

## Progresso de implementação (2026-07-16)

Pipeline completo implementado e validado ponta a ponta: `SensorSimulatorService` →
`IngestionService` → RabbitMQ → `ReadingWriterService` → Postgres. Detalhamento completo
em `Plano_SensorSimulatorService_Implementacao.md` e `Plano_Ingestion_Implementacao.md`.
Resumo do que já está no código:

- **`device_credentials`**: tabela + criptografia AES-256-GCM (`src/shared/crypto`) +
  capabilities internas `device_credential.get_active/rotate/revoke/list_active` no
  `device_service` + provisionamento atômico dentro de `device.create`
  (`device_credential.provision`, não é uma capability standalone). Testado com
  roundtrip de criptografia real via Postgres.
- **`devices.is_simulated`**: migration + trigger de imutabilidade (`V7`), aceito só em
  `device.create`.
- **`sensor_readings` idempotente**: `UNIQUE(sensor_id, timestamp)` (`V8`) +
  `SensorReadingRepository::insert()` via `ON CONFLICT DO NOTHING`.
- **Cliente/servidor CoAP/DTLS**: `libcoap` (backend OpenSSL) integrado via FetchContent
  — `CoapDtlsClient` (`src/shared/coap`, lado `SensorSimulatorService`) e o servidor PSK
  embutido no `IngestionService`. Handshake DTLS-PSK real validado por teste automatizado
  e ponta a ponta.
- **RabbitMQ**: `rabbitmq-c` vendorizado via FetchContent, wrapper
  `AmqpProducer`/`AmqpConsumer` (`src/shared/amqp`) — fila durável `sensor_readings`,
  uma mensagem por leitura.
- **`SensorSimulatorService`**: gera leituras periódicas em arquivo, transmite por CoAP/DTLS
  no intervalo configurado em `device_configurations` (ou via
  `POST /simulate/{device_id}/trigger`), buscando a credencial ativa a cada ciclo.
- **`IngestionService`**: servidor CoAP/DTLS stateless (sem Postgres), cache de PSKs em
  memória com refresh periódico (poll 60s via `device_credential.list_active`),
  validação mínima de formato, publica uma mensagem por leitura no RabbitMQ.
- **`ReadingWriterService`**: worker puro (sem gateway/capability), consome a fila e
  grava em `sensor_readings` com ack só após o insert confirmado.
- Validado de ponta a ponta com serviços reais rodando localmente (Postgres, gateway,
  device/field/farm_service, RabbitMQ, `IngestionService`, `ReadingWriterService`,
  `SensorSimulatorService`): `device.create` → geração → trigger manual → handshake
  DTLS-PSK → publish na fila → consumo → linha em `sensor_readings`, sem duplicação em
  disparos repetidos.

## Pontos em aberto

- **Validação contra faixas plausíveis** no `IngestionService` — ainda não feita. Desde
  2026-10 ele já confere dono do sensor e unidade (cache de `sensor.list_owners` com
  `unit`, Fase 0/1 do `Plano_Telemetria.md`) e a config do `weather_station` é validada no
  `DeviceConfigService` (schema + sensores do device). Falta rejeitar valores fisicamente
  impossíveis (ex.: umidade > 100 %) — onde definir as faixas (por tipo de sensor no
  código, ou em `sensors`) está em aberto.
- ✅ **Identidade pelo handshake** (2026-10-05) — device derivado da `psk_identity`,
  `device_id` divergente → `4.03`, `sensor_id` de outro device descartado. Fase 0 do
  `Plano_Telemetria.md`.
- **Invalidação de credencial em tempo real** — `IngestionService` usa poll periódico
  (60s) em vez do bridge de `device_credential.changed` via EventBus (que exigiria
  mudança no lado do gateway, fora de escopo por ora).
- Definir política de retry/DLQ no ReadingWriterService para mensagens que falham repetidamente.
  Ver "Vazão e falhas do ReadingWriterService" abaixo: hoje uma falha de banco derruba o
  processo em vez de chegar ao caminho "sem ack".
- Novos tipos em `sensors.type` (bateria, solar, agregados de vento) — `pressure` e `co2`
  já entraram (V9); os demais entram com o hardware (passos 5–6 do firmware). Ver seção
  "Amostragem interna vs. taxa de transmissão".
- Formato/tabela e conjunto inicial das regras de gatilho local (edge trigger) — ver seção correspondente acima.

## Vazão e falhas do ReadingWriterService (2026-10-09)

Visto no primeiro uplink com intervalo de 6 h da estação real: 1080 leituras em 7 arquivos
levaram ~10 min para sair da fila `sensor_readings` (~0,55 s por mensagem).

- ✅ **Espera fixa por fila** — o loop alternava `consumeOne(..., 500)` entre
  `sensor_readings` e `device_telemetry`. Com a fila de telemetria vazia, cada leitura
  custava ~500 ms de espera (teto de ~2 msg/s). Agora as duas filas são consultadas com
  timeout 0 e o loop só bloqueia 500 ms quando ambas estão vazias. O trabalho real
  (insert + commit + ack) ficou em ~50 ms por mensagem. Falta confirmar a vazão nova no
  próximo backlog.
- **Falha de banco derruba o serviço** — `PostgreSQLDatabase::execCommand` lança exceção
  em vez de retornar `false`, e `handleMessage`/`handleTelemetryMessage` não a capturam. O
  ramo "DB insert failed, leaving message unacked" nunca roda: o processo cai, o Docker
  reinicia (`restart: unless-stopped`) e o broker devolve as mensagens não confirmadas. Não
  perde dado, mas com o banco fora vira um loop de restart. Se o ramo passar a rodar, a
  mensagem fica sem ack nem nack, presa como unacked até a conexão cair. Decidir junto com a
  política de retry/DLQ: capturar a exceção, `basic_nack` com requeue e backoff, e
  dead-letter depois de N tentativas.
- **Sem `basic_qos`** — o consumer não limita o prefetch, então o broker entrega a fila
  inteira como unacked de uma vez. Um prefetch de ~100 basta.
- **Um commit por leitura** — juntar as mensagens disponíveis numa transação (ex.: até 100,
  ack com `multiple=1`) é o próximo ganho de vazão, se ~50 ms por mensagem não bastar.
- **Latência ociosa assimétrica** — com as filas vazias o loop bloqueia só no socket de
  `sensor_readings`, então telemetria que chega nesse momento espera até 500 ms. A solução
  completa é uma conexão só com os dois consumers e um único `amqp_consume_message` que
  separa por `consumer_tag`. Muda a API do `AmqpConsumer`; só vale se a latência importar.

## Observabilidade da fila

O `PLANO_GATEWAY_HTTP.md` (Fase 11) já define Grafana como ferramenta de observabilidade do sistema, unificando PostgreSQL (métricas/requests) e Loki (logs). RabbitMQ expõe métricas nativamente via plugin de management (Prometheus exporter desde a v3.8) — basta adicionar Prometheus como datasource no Grafana para visualizar profundidade de fila, taxa de consumo, conexões e crescimento de DLQ lado a lado com as métricas do gateway, sem introduzir ferramenta nova na stack.

## Ordem de implementação e dependências com o plano gateway

**Nota (2026-07-08, `Plano_Deployment.md` §6 passo 5):** RabbitMQ containerizado foi
adiado nesse plano de deployment — sem `IngestionService`/`ReadingWriterService`
implementados, não há nada publicando/consumindo a fila pra validar o container de
ponta a ponta. **Resolvido (2026-07-16)**: `docker-compose.qa-mq.yml` criado junto da
implementação dos dois serviços (`ingestion_service`/`reading_writer_service` em
`docker-compose.qa-app.yml`).

1. ✅ **Pipeline de ingestão primeiro, sem observabilidade**: simulador → IngestionService → RabbitMQ → ReadingWriterService → banco, validado ponta a ponta — ver "Progresso de implementação" acima. Observabilidade (passos 2-3 abaixo) ainda pendente.
2. **Fase 11 do gateway** (Grafana + Loki + Promtail): infraestrutura única e compartilhada — não faz sentido subir uma instância de Grafana separada para o ingestion.
3. **Plugar o ingestion na mesma instância**: adicionar Prometheus + RabbitMQ exporter como datasource no Grafana já existente.

## Dependência da Fase 10b do gateway (CI/CD)

A Fase 10b do `PLANO_GATEWAY_HTTP.md` define o pipeline de CI/CD (GitHub Actions + self-hosted runner + Docker) para o gateway. Os componentes deste plano entram no mesmo pipeline, em vez de terem um pipeline próprio:

- **IngestionService**, **ReadingWriterService** e **SensorSimulatorService** ganham estágios de build no `Dockerfile` multi-stage (ou Dockerfiles próprios), testados no mesmo `ci.yml` a cada PR.
- **RabbitMQ** entra como serviço de infraestrutura no compose (imagem oficial, sem build próprio), com config/provisionamento versionado (definições de fila, plugins management + Prometheus exporter habilitados).
- **Dashboards do Grafana** (painéis de fila/RabbitMQ) versionados como JSON e provisionados automaticamente no deploy, em vez de configurados manualmente na UI.
- O critério de aceite da Fase 10b ("merge na main → novo container em produção sem intervenção manual") passa a cobrir todo o pipeline de ingestão, não só o gateway.

---

## Amostragem interna vs. taxa de transmissão (2026-07-12)

**Contexto:** o usuário configura de 1 a 24 transmissões/dia por device. Amostrar o
sensor na mesma cadência da transmissão funciona bem para variáveis "lentas"
(temperatura, pressão, umidade, bateria), mas é insuficiente para variáveis que variam
rápido dentro do intervalo — o caso mais claro é vento, onde uma leitura pontual a cada
transmissão perde rajadas inteiras, justamente o dado mais relevante para decisões como
janela de pulverização.

**Decisão de design:** desacoplar as duas taxas.

- **Amostragem interna** (leitura do sensor pelo firmware): alta frequência para
  sensores voláteis (vento, a cada poucos segundos), baixa/pontual para sensores lentos
  (temperatura, pressão, umidade, bateria — uma leitura já representa bem o intervalo).
  Amostrar custa pouca energia comparado a transmitir (rádio é o maior consumidor), então
  não compete com o orçamento de bateria do device.
- **Transmissão** (o payload CoAP enviado): para sensores lentos, carrega o valor
  instantâneo mais recente. Para vento, carrega **agregados do intervalo desde a última
  transmissão**, não um valor pontual:
  - `wind_speed_avg` — velocidade média do intervalo.
  - `wind_speed_gust` — rajada máxima do intervalo (o dado que importa para decisão de
    pulverização/segurança, mais que a média).
  - `wind_direction` — direção predominante do intervalo, calculada por **média
    vetorial** (cada amostra convertida em vetor unitário, somada, ângulo resultante
    extraído) — média aritmética simples de ângulo é incorreta (ex.: média entre 350° e
    10° não é 180°, é 0°).

**Impacto no modelo de dados:** cabe como extensão do `ENUM` de `sensors.type` em
`Plano_DB_IOT_Sensors.md`, sem mudança em `sensor_readings` — cada agregado de vento
(`wind_speed_avg`, `wind_speed_gust`, `wind_direction`) é um sensor lógico próprio
atrelado ao mesmo device físico, mesmo padrão já usado para bateria/solar:

```sql
type : ENUM('temperature', 'moisture', 'ph', 'humidity', 'luminosity',
            'battery_level', 'battery_voltage', 'solar_panel_voltage',
            'wind_speed_avg', 'wind_speed_gust', 'wind_direction', 'other')
```

- **Pendente**: atualizar esse `ENUM` em `Plano_DB_IOT_Sensors.md` (este documento só
  registra a decisão; a migration em si pertence àquele plano).

## Envio antecipado por gatilho local (edge trigger) (2026-07-12)

**Contexto:** com transmissão de até 1x/dia (o mínimo configurável), uma condição que
merece alarme (geada, rajada perigosa, etc.) pode ocorrer logo após uma transmissão e só
ser reportada até 24h depois — tarde demais para qualquer ação do produtor. O
`Plano_Alerting.md` só avalia o que já chegou ao servidor; se o dado não chega, não tem
o que avaliar.

**Decisão de design:** o firmware mantém um conjunto pequeno de **regras de gatilho
locais** — thresholds simples (sem correlação entre sinais, sem histerese sofisticada,
sem janela deslizante — isso continua sendo responsabilidade do `AlertingService` do
lado servidor) avaliados a cada amostragem interna (a mesma amostragem de alta
frequência já descrita acima para vento, e também aplicável a temperatura quando a
amostragem lenta ainda for suficiente para captar uma queda rápida). Ao violar uma
regra local, o device dispara uma transmissão **imediata, fora do agendamento regular**
— sem esperar o próximo ciclo.

- **Sincronização das regras locais, sem canal de downlink dedicado:** carona na
  resposta (ACK) de cada transmissão regular — o `IngestionService` devolve, junto da
  confirmação CoAP, a versão atual das regras de gatilho relevantes para aquele device
  (subconjunto simplificado de `alarm_rules`, ou uma tabela própria mais enxuta — a
  decidir). Evita manter um mecanismo de polling/Observe só para isso; o device já fala
  com o servidor a cada ciclo de qualquer forma.
- **Cooldown local, no próprio firmware:** evita que uma condição que oscila perto do
  limite (ex. vento variando ao redor do threshold de rajada) esgote a bateria disparando
  transmissão extra repetidamente — intervalo mínimo entre disparos de gatilho,
  independente do cooldown que já existe no `NotificationService` (`Plano_Alerting.md`),
  que é uma camada de proteção diferente (evita SMS repetido, não transmissão repetida).
- **Sem duplicar a lógica do `AlertingService`:** a leitura enviada por gatilho entra no
  pipeline exatamente como qualquer outra (`IngestionService` → fila →
  `ReadingWriterService` → banco → Camada 1/2 do `Plano_Alerting.md`), só que mais cedo.
  O firmware não decide se é alarme — só decide que aquela leitura não pode esperar o
  próximo ciclo regular. A avaliação completa (histerese, correlação, notificação)
  continua sendo feita do lado servidor, como já desenhado.
- Marcar o payload de gatilho com um campo simples (`trigger: true` ou similar) ajuda o
  `IngestionService`/`AlertingService` a diferenciar de uma leitura regular, se algum
  tratamento prioritário for necessário mais adiante (não obrigatório para o MVP).

**Pontos em aberto (gatilho local):**
- Formato/tabela das regras de gatilho locais — subconjunto simplificado de
  `alarm_rules` ou tabela própria (`edge_trigger_rules`?) mais enxuta.
- Conjunto inicial de condições que valem gatilho local (geada é o caso óbvio; rajada de
  vento perigosa outro) vs. o que pode esperar o ciclo regular.
- Duração do cooldown local no firmware, e se é fixo ou configurável por device/regra.
