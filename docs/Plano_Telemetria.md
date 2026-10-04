# Plano — Modelo de Dados de Telemetria (Estação ↔ Backend)

## Contexto

Hoje o `IngestionService` (PR #83) espera, via CoAP/DTLS, um `POST` na raiz com body JSON
(Content-Format `50`):

```json
{ "device_id": "1234", "readings": [{ "sensor_id": "1", "timestamp": "2026-10-02T12:10:00Z", "value": 25.4, "unit": "°C" }] }
```

Restrições do parser atual (`json_helper.cpp`), que já pegaram o firmware:

- `device_id` e `sensor_id` precisam ser **string**. Com `device_id` numérico o payload
  inteiro é rejeitado (`4.00`); com `sensor_id` numérico a leitura é descartada.
- `value` é lido com `IsDouble()`: um inteiro no JSON (`"value": 20`) é descartado.
- `device_id` vem do corpo e não é conferido contra a PSK do handshake.

A resposta é só o código (`2.04`/`4.00`), sem corpo.

Esse formato atende o `SensorSimulatorService` e o firmware atual (`rdws_weather_node`,
passo 3), mas não cobre alguns pontos que o firmware real da estação precisa:

- envio em lote de várias janelas de amostragem (backlog no LittleFS, NB-IoT com
  transmissão de até 1x/dia);
- dados de diagnóstico do device (RSSI, SNR, reinicializações);
- metadados de pacote (`seq`, `trigger` do edge trigger);
- futuramente, estações que agregam sensores sem fio (ZigBee, BLE).

Este documento define o formato de payload e a forma de persistência dessas categorias.

## Proposta inicial e problemas identificados

A primeira proposta separava `sensors` (medições) de `device` (saúde do device) num JSON
com chaves por nome (`"temperature": 25.4`) e um único `timestamp` por mensagem. A ideia
de separar medição de diagnóstico foi mantida. Os pontos que motivaram a revisão:

1. **Chave por nome de grandeza** impede dois sensores do mesmo tipo no mesmo device
   (ex.: sondas de umidade do solo em profundidades diferentes) e conflita com o sensor
   engine extensível. O backend persiste por `sensor_id`; tipo e unidade já estão em
   `sensors`/`device_config`.
2. **Timestamp único por mensagem** não suporta envio em lote de várias janelas.
3. **Bateria no bloco de diagnóstico** reabriria a decisão de modelar bateria/solar como
   sensores lógicos (`battery_voltage`, `solar_panel_voltage`), que existe para que o
   alerting possa avaliá-los.
4. **`station_id` no corpo** não é fonte de identidade confiável. A identidade real é a
   PSK identity do DTLS.

## Decisões

### D1 — Formato de payload: SenML (RFC 8428)

- Payload em **SenML JSON** (CoAP Content-Format `110`, `application/senml+json`).
- Migração futura para **SenML CBOR** (Content-Format `112`, `application/senml+cbor`)
  quando o tamanho do payload pesar no NB-IoT. Zephyr tem `zcbor`. A estrutura é a mesma,
  só muda a codificação.
- Estrutura plana, sem aninhamento: `bn` (base name) identifica o device (ver **DP2**:
  pode ser omitido), `bt` (base time, epoch) ancora o tempo e `t` é relativo ao `bt`.
  Várias janelas cabem no mesmo pacote.
- O parser deve aceitar qualquer número em `v` (`IsNumber()`), não só `IsDouble()` como
  o parser atual: no exemplo abaixo, `98780` e `812` são inteiros.
- Ganho de tamanho em relação ao formato atual: o timestamp ISO de 20 caracteres repetido
  em cada leitura vira um `t` relativo curto, então cabem mais registros por pacote.

Exemplo:

```json
[
  {"bn":"1234/", "bt":1791035100, "n":"seq", "v":812},
  {"n":"trigger", "vb":false},
  {"n":"1", "u":"Cel", "v":25.4},
  {"n":"2", "u":"%RH", "v":62.1},
  {"n":"3", "u":"Pa",  "v":98780},
  {"n":"7", "u":"V",   "v":3.912},
  {"n":"rssi", "v":-67},
  {"n":"1", "t":600, "v":25.1},
  {"n":"2", "t":600, "v":61.8}
]
```

### D2 — Nomes (`n`)

- **Sensores**: `n` é um **id local** do sensor, definido em `device_config`. O
  `IngestionService` traduz id local → `sensor_id` global. Isso desacopla o firmware dos
  ids do banco.
  - Efeito de segurança: um id local só existe dentro do device autenticado (D4), então a
    estação não consegue gravar em sensor de outro device. Hoje consegue, porque manda
    `sensor_id` global.
  - Resolve o mapeamento canal → `sensor_id` fixo no build do firmware (ponto em aberto
    do `Plano_Firmware_WeatherNode.md`).
  - O `IngestionService` não acessa o banco: manter um **cache em memória do mapeamento
    id local → `sensor_id` por device**, recarregado por poll periódico via capability,
    no mesmo padrão do cache de PSK (`device_credential.list_active`).
- **Diagnóstico**: nomes reservados (`rssi`, `snr`, `boot_count`, `reset_reason`, …).
- **Metadados de pacote**: o SenML não tem campos de cabeçalho. `seq` e `trigger` viajam
  como registros (`{"n":"seq","v":...}`, `{"n":"trigger","vb":true}`).

### D3 — Unidades (em aberto, ver DP1)

- Seguir o registro IANA de unidades SenML (`Cel`, `%RH`, `Pa`, `V`, …) no payload.
- **Contradição a resolver:** a proposta original normalizava a pressão em `Pa` no firmware
  e mantinha `sensors.unit` como referência, com `u` opcional. Mas `sensors.unit` usa
  outra nomenclatura (`kPa` decidido no RDWS-103, `%` em vez de `%RH`, `°C` em vez de
  `Cel`). Sem uma regra de conversão, um valor em `Pa` gravado num sensor em `kPa` fica
  1000× errado sem erro visível. Opções em **DP1**.

### D4 — Identidade

- O device é derivado da **PSK identity** do DTLS. O `bn` é validado contra ela: se
  divergir, a mensagem é rejeitada. Um device com credencial válida não pode escrever em
  nome de outro.

### D5 — Diagnóstico como série temporal, em tabela própria

Diagnóstico (RSSI, SNR, reinicializações…) também é série temporal, mas vai para uma
tabela separada de `sensor_readings`:

```sql
CREATE TABLE device_telemetry (
  device_id  BIGINT NOT NULL REFERENCES devices(id),
  timestamp  TIMESTAMPTZ NOT NULL,
  data       JSONB NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  UNIQUE (device_id, timestamp)
) PARTITION BY RANGE (timestamp);
```

Motivos:

- **Sem cadastro**: não exige uma linha em `sensors` por métrica de diagnóstico por
  device.
- **Retenção própria**: diagnóstico interessa por 30 a 90 dias; dado agronômico, anos.
- **Ritmo natural**: o diagnóstico é um snapshot por transmissão, então uma linha por
  envio com JSONB representa bem.
- **Schema flexível**: o firmware pode adicionar métricas sem migration.
- **Idempotência**: `UNIQUE(device_id, timestamp)`, mesmo padrão de `sensor_readings`.

Trade-off aceito: queries do tipo `(data->>'rssi')::int` e sem validação de tipo no
banco. Se alguma métrica virar consulta frequente, criar índice de expressão ou
promovê-la a coluna.

Particionamento: o `UNIQUE (device_id, timestamp)` é válido na tabela particionada porque
inclui a chave de partição. Falta definir **quem cria as partições futuras e derruba as
antigas** (retenção): `pg_partman`, um job agendado ou uma migration periódica. O mesmo
problema existe para `sensor_readings` (`Plano_DB_IOT_Sensors.md`), então a solução deve
ser uma só para as duas tabelas.

### D6 — Bateria e painel solar permanecem em `sensor_readings`

Apesar de serem diagnóstico, geram alarme ("bateria baixa") e interessam ao produtor no
dashboard. Continuam como sensores lógicos, para que o `AlertingService` avalie uma única
fonte.

### D7 — Uptime substituído por eventos de boot

Uptime é um contador que zera e cresce linearmente, e armazená-lo a cada envio é
majoritariamente ruído. Enviar `boot_count` e `reset_reason`, que mostram quando e por
que o device reiniciou.

### D8 — Roteamento no `IngestionService`

- Registros com `n` numérico (id local de sensor) → `sensor_readings` (via fila
  `sensor_readings`, como hoje).
- Registros com nome reservado de diagnóstico → agrupados por timestamp resolvido numa
  linha de `device_telemetry`.
- `seq`/`trigger` → metadados da mensagem (log, detecção de perda, priorização futura).
- O firmware não conhece essa separação.

### D9 — Diagnóstico do sistema de arquivos (LittleFS)

Métricas enviadas como nomes reservados de diagnóstico, persistidas em
`device_telemetry` (sem migration, via JSONB):

- **`fs_used_pct`**: ocupação geral do FS. O FS guarda backlog, config e logs, então a
  tendência ao longo de semanas revela vazamento de espaço ou log sem rotação.
- **`fs_errors`**: contador de falhas de escrita ou montagem.
- **`fs_reformat`**: evento enviado quando o firmware reformata o FS (ex.: ao detectar
  corrupção). Significa dado perdido, então é tratado como evento, no mesmo espírito de
  `reset_reason`.
  - **Hoje não é detectável**: o firmware monta o LittleFS por `automount` no fstab, e o
    Zephyr formata sozinho quando a montagem falha (aconteceu no primeiro boot da
    bancada). Para emitir o evento, montar com `FS_MOUNT_FLAG_NO_FORMAT`, detectar a
    falha e formatar explicitamente, registrando o evento.
- **`backlog_count`** (condicional): só se o firmware puder esvaziar o backlog
  **parcialmente** numa sessão (limite de payload por pacote no NB-IoT, limite de
  registros por ciclo para poupar bateria, queda de conexão no meio do envio). Nesse caso
  indica quanto ficou pendente. Se o envio sempre esvazia tudo, não é enviado.

**Descartado:**
- **`backlog_oldest_s`**: o device só reporta quando consegue transmitir, que é quando
  esvazia a fila, então o valor seria sempre trivial. A informação útil (quanto atraso o
  backlog acumulou) o backend já obtém das próprias leituras: `created_at - timestamp`.
- **Contagem de desgaste da flash**: o LittleFS faz wear leveling, mas não expõe contagem
  de erase por bloco de forma simples.

**Custo da coleta:** no Zephyr, `fs_statvfs` no LittleFS chama `lfs_fs_size`, que
percorre o FS inteiro. Coletar uma vez por transmissão, nunca a cada amostragem.

### D10 — Estação silenciosa e atraso de ingestão são detectados no servidor

Uma estação sem conectividade não consegue reportar que está sem conectividade. Logo:

- **Estação silenciosa**: detectada pela **ausência** de dados. Manter um `last_seen` por
  device e uma regra do lado servidor que sinaliza devices sem envio além de um limite
  (ex.: N × intervalo de transmissão configurado).
  - Custo da proposta original (`last_seen` atualizado a cada mensagem aceita): o
    `IngestionService` não acessa o banco, então seria uma capability via broker por
    mensagem (como `device.update_location`), mais um `UPDATE` em `devices` a cada envio.
  - Alternativas mais baratas: derivar de `max(created_at)` de `sensor_readings` e
    `device_telemetry` por device (sem escrita extra; índice já existe), ou atualizar
    `last_seen` só quando o valor gravado tiver mais de N minutos.
- **Atraso de ingestão**: calculado a partir de `created_at - timestamp` das leituras
  recebidas. Picos indicam que a estação ficou um período sem conseguir enviar e
  descarregou o backlog.
- Esses alertas são **de frota** (operação/manutenção), não do produtor. Ficam separados
  do `AlertingService`, que avalia `sensor_readings`.

### D11 — Corpo da resposta CoAP

O contrato vai mudar de qualquer jeito, então definir junto o corpo do ACK, hoje vazio:
`config_version` e `rules_version` (piggyback, `Plano_Ingestion.md` e
`Plano_Firmware_WeatherNode.md` §3). É pré-requisito do passo 4 do firmware (config vinda
do backend). Formato do corpo (SenML, JSON curto ou CBOR) a definir junto com DP1–DP3.

## Decisões pendentes (DP)

### DP1 — Referência de unidades (D3)

- **(a) O servidor converte.** Tabela de conversão SenML ↔ `sensors.unit` no
  `IngestionService` (`Pa` → `kPa` ÷ 1000, `Cel` → `°C`, `%RH` → `%`). `u` passa a ser
  obrigatório para sensores; unidade sem regra de conversão é rejeitada. O banco não muda.
- **(b) O banco adota SenML.** `sensors.unit` passa a usar a nomenclatura SenML, com
  migration nos dados existentes (`kPa` → `Pa` multiplicando `sensor_readings.value`
  por 1000, `°C` → `Cel`, `%` → `%RH`). O firmware envia exatamente a unidade
  cadastrada, e o servidor só confere `u` = `sensors.unit`.
- Em ambos os casos, `u` divergente é erro explícito, nunca gravado silenciosamente.

### DP2 — `bn` no device principal (D1, D4)

Com o device derivado da PSK (D4), o `bn` com o id do device é redundante, e obriga o
firmware a conhecer o próprio `device_id` (hoje `CONFIG_RDWS_DEVICE_ID`, que se quer
eliminar).

- **(a) Omitir `bn`** no device principal; o servidor usa a identidade da PSK. `bn` só
  aparece para nós filhos no cenário de agregador (`"bn":"ble-a3/"`, relativo à estação).
- **(b) Manter `bn` com o id do device**, validado contra a PSK como proposto no D4.
  Redundância deliberada, ao custo de bytes e de provisionar o id no firmware.

### DP3 — Flags por registro (`trigger`, `partial_window`, `time_unsynced`)

O firmware marca flags **por registro** (`struct record.flags`): qual leitura disparou o
edge trigger, janela incompleta, timestamp sem relógio. A proposta original leva `trigger`
como registro de pacote (`{"n":"trigger","vb":true}`), que não diz qual leitura disparou,
e não tem lugar para as outras duas.

- **(a) Campo de extensão por registro.** O SenML permite campos próprios; terminados em
  `_` quando o receptor é obrigado a entendê-los (RFC 8428 §4.4). Ex.: `"fl_":4` com o
  mesmo bitmask do firmware. Registros sem flags não levam o campo.
- **(b) Registros de pacote**, como na proposta original, aceitando perder a associação
  com a leitura. `time_unsynced` não seria enviado (o firmware já não envia hoje).

## Futuro — estação como agregadora de sensores sem fio (fora do escopo deste device)

- **Payload**: cada nó filho é outro `bn` no mesmo pacote SenML (ex.:
  `"bn":"1234/ble-a3/"`). Sem JSON aninhado: o parser continua linear no firmware e no
  backend. Diagnóstico do nó filho (bateria do nó, RSSI do enlace BLE/ZigBee) segue o
  mesmo padrão.
- **Banco**: `devices.type` já tem `gateway`. Adicionar `parent_device_id` em `devices`
  para modelar estação → nós filhos.
- **Confiança**: o DTLS autentica só a estação, não os nós. O `IngestionService` precisa
  validar que o `bn` filho pertence ao gateway autenticado (`parent_device_id` = device
  autenticado). Sem isso, uma estação comprometida pode injetar leituras em qualquer
  device.

## Fases

### Fase 0 — Identidade pela PSK (antes do SenML)
Independente do formato, e pré-requisito de segurança. Pode valer já para o JSON atual.
- ⬜ `IngestionService`: obter a `psk_identity` da sessão DTLS e resolver o device.
- ⬜ `IngestionService`: rejeitar `sensor_id` que não pertença ao device autenticado
  (formato atual) e, no formato atual, ignorar ou conferir o `device_id` do corpo.

### Fase 1 — Contrato e backend
- ⬜ Decidir DP1, DP2 e DP3.
- ⬜ Definir o corpo da resposta CoAP (D11).
- ⬜ Definir lista de nomes reservados de diagnóstico.
- ⬜ Definir como o id local de sensor é declarado em `device_config`.
- ⬜ Migration Flyway: `device_telemetry` particionada, com política de retenção.
- ⬜ Mecanismo de criação/retenção de partições, comum a `sensor_readings` (D5).
- ⬜ `IngestionService`: parser SenML JSON (Content-Format `110`), mantendo o formato JSON
  atual durante a transição (despacho por Content-Format).
- ⬜ `IngestionService`: identidade do SenML conforme DP2 (a partir da Fase 0).
- ⬜ `IngestionService`: cache id local → `sensor_id` por device (D2).
- ⬜ `IngestionService`: unidades conforme DP1.
- ⬜ `IngestionService`: tradução id local → `sensor_id` e roteamento (D8).
- ⬜ `IngestionService`: atualizar `devices.last_seen` a cada mensagem aceita (D10).
- ⬜ `last_seen`: coluna `devices.last_seen` ou derivado das leituras (D10).
- ⬜ `ReadingWriterService` (ou consumer dedicado): escrita idempotente em
  `device_telemetry`.

### Fase 2 — Simulador e firmware
- ⬜ `SensorSimulatorService` gerando SenML, incluindo diagnóstico e múltiplas janelas.
- ⬜ Firmware: encoder SenML JSON com `bt`/`t`, `seq`, `trigger` e diagnóstico.
- ⬜ Firmware: coleta de `fs_used_pct`, `fs_errors` e evento `fs_reformat`, uma vez por
  transmissão (D9). O `fs_reformat` exige trocar o automount por montagem explícita.
- ⬜ Firmware: ids locais (D2) no lugar dos `sensor_id` do Kconfig, e sem `device_id`
  conforme DP2.
- ⬜ Teste ponta-a-ponta: firmware/simulador → `IngestionService` → fila → banco, sem
  duplicação em reenvio.

### Fase 3 — Observabilidade
- ⬜ Regra de frota para estação silenciosa, baseada em `last_seen` (D10).
- ⬜ Painel de atraso de ingestão (`created_at - timestamp`) por device (D10).
- ⬜ Painéis Grafana de saúde da frota (RSSI/SNR por device, reinicializações, ocupação e
  erros do FS), versionados como JSON.

### Fase 4 — CBOR (quando necessário)
- ⬜ Encoder SenML CBOR no firmware (`zcbor`) e parser no `IngestionService`
  (Content-Format `112`).

## Pontos em aberto

- Acesso do `IngestionService` (stateless) ao `device_config` para traduzir ids locais.
  Proposta em D2 (cache por poll); depende da pendência já registrada em
  `Plano_Ingestion.md` sobre validação contra `device_config`.
- DP1, DP2 e DP3 (decisões pendentes acima).
- Formato do corpo da resposta CoAP (D11).
- Mecanismo de partições e retenção, comum a `sensor_readings` e `device_telemetry` (D5).
- Período de retenção de `device_telemetry`.
- Quando desativar o formato JSON legado no `IngestionService`.
- Se `seq` é persistido (para métricas de perda de pacotes) ou só logado.
- Se vale uma coluna `category` em `sensors` para separar sensores lógicos de saúde
  (bateria/solar) dos ambientais no dashboard do produtor.
- Se o firmware pode esvaziar o backlog parcialmente numa sessão, o que decide se
  `backlog_count` é enviado (D9).
- Limite para considerar uma estação silenciosa (múltiplo fixo do intervalo de
  transmissão ou configurável por device) e onde a regra de frota roda (D10).
- Canal de notificação dos alertas de frota, separado das notificações ao produtor.
