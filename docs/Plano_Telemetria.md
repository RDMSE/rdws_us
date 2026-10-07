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
- Estrutura plana, sem aninhamento (array de registros; o `"e"` do rascunho pré-RFC não
  existe na RFC 8428): `bn` (base name) identifica o device (**DP2**, decidido: mantido),
  `bt` (base time, epoch) ancora o tempo e `t` é relativo ao `bt`. Várias janelas cabem
  no mesmo pacote.
- O parser deve aceitar qualquer número em `v` (`IsNumber()`), não só `IsDouble()` como
  o parser atual: no exemplo abaixo, `98780` e `812` são inteiros.
- Ganho de tamanho em relação ao formato atual: o timestamp ISO de 20 caracteres repetido
  em cada leitura vira um `t` relativo curto, então cabem mais registros por pacote.

Exemplo:

```json
[
  {"bn":"12/", "bt":1791035100, "n":"seq", "v":812},
  {"n":"31", "u":"Cel", "v":25.4, "fl_":2},
  {"n":"32", "u":"%RH", "v":62.1},
  {"n":"33", "u":"Pa",  "v":98780},
  {"n":"37", "u":"V",   "v":3.912},
  {"n":"rssi", "v":-67},
  {"n":"31", "t":600, "v":25.1},
  {"n":"32", "t":600, "v":61.8}
]
```

### D2 — Nomes (`n`) 

O nome completo de um registro é `bn` + `n` (RFC 8428 §4.5.1). O servidor confere o
prefixo `<device_id>/` contra a PSK (D4) e classifica o restante:

- **Sensores**: `n` numérico é o **`sensor_id` global** do banco (F1: o id local
  proposto antes foi abandonado). Os dois motivos dele já estão cobertos: a segurança
  pelo descarte de sensor de outro device (Fase 0) e o mapeamento fixo no build pela
  config vinda do backend (passo 4 do firmware), que já usa `sensor_id` global.
- **Metadados de pacote**: o SenML não tem campos de cabeçalho. `seq` viaja como registro
  (`{"n":"seq","v":...}`). O `trigger` deixou de ser registro de pacote e virou flag do
  registro que disparou (DP3).
- **Diagnóstico**: qualquer outro nome. Os conhecidos (2026-10-05):

  | `n`             | Tipo | Unidade implícita | Significado                               |
  |-----------------|------|-------------------|-------------------------------------------|
  | `rssi`          | `v`  | dBm               | Sinal do enlace (Wi-Fi/NB-IoT), D5        |
  | `snr`           | `v`  | dB                | Relação sinal-ruído (NB-IoT), D5          |
  | `boot_count`    | `v`  | contagem          | Contador de boots, D7                     |
  | `reset_reason`  | `v`  | bitmask           | Causa do reset (`hwinfo` do Zephyr), D7   |
  | `fs_used_pct`   | `v`  | % (0–100)         | Ocupação do LittleFS, D9                  |
  | `fs_errors`     | `v`  | contagem          | Falhas de escrita/montagem, D9            |
  | `fs_reformat`   | `vb` | evento            | FS reformatado (dado perdido), D9         |
  | `backlog_count` | `v`  | registros         | Pendentes, só com envio parcial (D9)      |
  | `cfg_version`   | `v`  | versão            | Config que o device roda (D11)            |

  - Diagnóstico vai **sem `u`**: a unidade é fixa por nome (RFC 8428 permite registro
    sem unidade quando o contexto a define). Evita `dBm` (só existe como unidade
    secundária, RFC 8798) e `%` (não recomendado no registro SenML).
  - Nome fora da tabela não é erro: vai para `device_telemetry` mesmo assim (o JSONB do D5
    existe para o firmware poder adicionar métricas sem mudar o servidor), e a tabela
    acima é atualizada quando a métrica ganhar uso.

### D3 — Unidades (DP1 decidido: o servidor converte)

- Seguir o registro IANA de unidades SenML (`Cel`, `%RH`, `Pa`, `V`, …) no payload.
- O SenML usa a grafia do UCUM, mas só aceita os símbolos do registro dele: prefixos SI
  são desaconselhados (RFC 8428 §12.1) e o valor vai na unidade base (`98780` em `Pa`, não
  `98.78` em `kPa`). `kPa` não existe no registro, nem como unidade secundária da RFC 8798
  (que tem `hPa`, mas cujo uso exige SenML versão nova ou campo must-understand). `°C` não
  é símbolo nem do SenML nem do UCUM (`Cel`).
- `sensors.unit` usa outra nomenclatura (`kPa` decidido no RDWS-103, `%` em vez de `%RH`,
  `°C` em vez de `Cel`). Por isso o `IngestionService` converte (DP1, opção a); o banco e
  os dashboards não mudam.

### D4 — Identidade

- O device é derivado da **PSK identity** do DTLS. O `bn` é validado contra ela: se
  divergir, a mensagem é rejeitada. Um device com credencial válida não pode escrever em
  nome de outro.
- O `bn` com o id do device é redundante, mas fica (DP2, opção b): facilita ler pacotes
  no debug. No firmware, o id deixa de vir do Kconfig e passa a ser provisionado pelo
  shell (`rdws id set <id>`, em `settings`, como a PSK).

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
);
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

Particionamento (F2, 2026-10-05): **a tabela nasce sem partição**, como `sensor_readings`.
O volume é de uma linha por transmissão por device. Particionar e reter as duas tabelas é
um item próprio, depois (Fase 3): falta definir **quem cria as partições futuras e
derruba as antigas** (`pg_partman`, um job agendado ou uma migration periódica), e a
solução deve ser uma só para as duas. O `UNIQUE (device_id, timestamp)` continua válido
quando particionar, porque inclui a chave de partição.

### D6 — Bateria e painel solar permanecem em `sensor_readings`

Apesar de serem diagnóstico, geram alarme ("bateria baixa") e interessam ao produtor no
dashboard. Continuam como sensores lógicos, para que o `AlertingService` avalie uma única
fonte.

### D7 — Uptime substituído por eventos de boot

Uptime é um contador que zera e cresce linearmente, e armazená-lo a cada envio é
majoritariamente ruído. Enviar `boot_count` e `reset_reason`, que mostram quando e por
que o device reiniciou.

### D8 — Roteamento no `IngestionService`

- Registros com `n` numérico (`sensor_id`) → `sensor_readings` (via fila
  `sensor_readings`, como hoje), depois de conferir o dono (Fase 0) e converter a
  unidade (DP1).
- Registros com nome reservado de diagnóstico → agrupados por timestamp resolvido numa
  linha de `device_telemetry`.
- `seq` → metadado da mensagem, gravado no `data` de `device_telemetry` junto do
  diagnóstico do mesmo instante (decidido em 2026-10-05), para detectar perda depois.
- `fl_` → flags da leitura (DP3), gravadas em `sensor_readings.flags` (F3). Um registro
  com o bit de trigger marca a mensagem para priorização futura.
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
- `last_seen` foi para a **Fase 3** (F4, 2026-10-05), junto da regra que o consome.

### D11 — Corpo da resposta CoAP

O contrato vai mudar de qualquer jeito, então definir junto o corpo do ACK, hoje vazio:
`config_version` e `rules_version` (piggyback, `Plano_Ingestion.md` e
`Plano_Firmware_WeatherNode.md` §3). É pré-requisito do passo 4 do firmware (config vinda
do backend).

~~Decidido (2026-10-05): JSON curto `{"cfg":3,"rul":1}` com as versões, e `GET /config`
quando divergirem.~~ **Revisto em 2026-10-06:** o device declara a versão que tem e a
config viaja na própria resposta, só quando mudou:

- O device manda `cfg_version` como diagnóstico (D2), no primeiro pacote aceito de cada
  ciclo (o mesmo que leva `boot_count`/`reset_reason`/`rssi`).
- Se `cfg_version` diferir de `device_configurations.version`, o `2.04` **desse pacote**
  traz a config completa (JSON, Content-Format `50`, Block2 se não couber num PDU).
  Versão igual, ou pacote sem `cfg_version`: `2.04` sem corpo, como hoje.
- O device guarda a config recebida e a aplica no fim do ciclo (ponto de sincronização,
  `Plano_Firmware_WeatherNode.md` §5).
- Uma ida e volta só, sem `GET /config`; a config só trafega quando muda; e o servidor
  sabe qual versão cada device roda (vai para `device_telemetry` como qualquer
  diagnóstico).
- Compatível nos dois sentidos: firmware antigo, JSON legado e simulador não mandam
  `cfg_version` e nunca recebem config.
- Regras de edge trigger (`rul`): mesmo mecanismo quando existirem (passo 8 do firmware).
- Sem RTC acertado o diagnóstico não vai e a config não é atualizada naquele ciclo —
  mas sem RTC os registros também não saem.

## Decisões de contrato (DP, decididas em 2026-10-05)

Decisão em cada item; as opções ficam registradas pelo histórico.

### DP1 — Referência de unidades (D3) → (a) o servidor converte

- **(a) O servidor converte.** Tabela de conversão SenML ↔ `sensors.unit` no
  `IngestionService` (`Pa` → `kPa` ÷ 1000, `Cel` → `°C`, `%RH` → `%`). `u` passa a ser
  obrigatório para sensores; unidade sem regra de conversão é rejeitada. O banco não muda.
- **(b) O banco adota SenML.** `sensors.unit` passa a usar a nomenclatura SenML, com
  migration nos dados existentes (`kPa` → `Pa` multiplicando `sensor_readings.value`
  por 1000, `°C` → `Cel`, `%` → `%RH`). O firmware envia exatamente a unidade
  cadastrada, e o servidor só confere `u` = `sensors.unit`.
- Em ambos os casos, `u` divergente é erro explícito, nunca gravado silenciosamente.

Tabela de conversão (2026-10-05, a partir das unidades cadastradas no `rdws_qa`;
`rdws::senml::toSensorUnit` em `src/shared/senml/conversion.cpp`):

| SenML (`u`) | `sensors.unit` | Fator | Tipo |
|---|---|---|---|
| `Cel` | `°C` | ×1 | temperature |
| `%RH` | `%` | ×1 | humidity |
| `/` (razão 0–1) | `%` | ×100 | moisture |
| `Pa` | `kPa` | ÷1000 | pressure |
| `lx` | `lux` | ×1 | luminosity |
| igual ao cadastrado (ex.: `pH`) | idem | ×1 | qualquer |

Sem `u`, ou par sem regra: o registro é descartado com log e o resto do pacote segue
(`2.04`), como na Fase 0.

### DP2 — `bn` no device principal (D1, D4) → (b) manter o id do device

Com o device derivado da PSK (D4), o `bn` com o id do device é redundante, e obriga o
firmware a conhecer o próprio `device_id` (hoje `CONFIG_RDWS_DEVICE_ID`, que se quer
eliminar).

- **(a) Omitir `bn`** no device principal; o servidor usa a identidade da PSK. `bn` só
  aparece para nós filhos no cenário de agregador (`"bn":"ble-a3/"`, relativo à estação).
- **(b) Manter `bn` com o id do device**, validado contra a PSK como proposto no D4.
  Redundância deliberada, ao custo de bytes e de provisionar o id no firmware.

### DP3 — Flags por registro (`trigger`, `partial_window`, `time_unsynced`) → (a) `fl_`

O firmware marca flags **por registro** (`struct record.flags`): qual leitura disparou o
edge trigger, janela incompleta, timestamp sem relógio. A proposta original leva `trigger`
como registro de pacote (`{"n":"trigger","vb":true}`), que não diz qual leitura disparou,
e não tem lugar para as outras duas.

- **(a) Campo de extensão por registro.** O SenML permite campos próprios; terminados em
  `_` quando o receptor é obrigado a entendê-los (RFC 8428 §4.4). Ex.: `"fl_":4` com o
  mesmo bitmask do firmware. Registros sem flags não levam o campo.
- **(b) Registros de pacote**, como na proposta original, aceitando perder a associação
  com a leitura. `time_unsynced` não seria enviado (o firmware já não envia hoje).

No servidor (F3, 2026-10-05):

- Bits: `0x1` `time_unsynced`, `0x2` `trigger`, `0x4` `partial_window` (os mesmos de
  `RECORD_FLAG_*` em `rdws_weather_node/src/sensing/record.h`).
- Gravado em `sensor_readings.flags SMALLINT NOT NULL DEFAULT 0`: janela parcial é
  informação de qualidade do dado, útil no dashboard.
- Registro com `time_unsynced` é **descartado com log**: sem relógio, o timestamp não
  significa nada.
- Bit desconhecido é erro do registro (descartado com log), coerente com o `_` de
  must-understand.

## Decisões de implementação da Fase 1 (F, 2026-10-05)

- **F1**: sem id local de sensor; `n` é o `sensor_id` global (D2).
- **F2**: `device_telemetry` sem partição por enquanto (D5).
- **F3**: `fl_` gravado em `sensor_readings.flags`; `time_unsynced` descartado (DP3).
- **F4**: `last_seen` movido para a Fase 3 (D10).

## Futuro — estação como agregadora de sensores sem fio (fora do escopo deste device)

- **Payload**: cada nó filho é outro `bn` no mesmo pacote SenML (ex.:
  `"bn":"12/ble-a3/"`). Sem JSON aninhado: o parser continua linear no firmware e no
  backend. Diagnóstico do nó filho (bateria do nó, RSSI do enlace BLE/ZigBee) segue o
  mesmo padrão.
- **Banco**: `devices.type` já tem `gateway`. Adicionar `parent_device_id` em `devices`
  para modelar estação → nós filhos.
- **Confiança**: o DTLS autentica só a estação, não os nós. O `IngestionService` precisa
  validar que o `bn` filho pertence ao gateway autenticado (`parent_device_id` = device
  autenticado). Sem isso, uma estação comprometida pode injetar leituras em qualquer
  device.

## Fases

### Fase 0 — Identidade pela PSK (antes do SenML) ✅
Independente do formato, e pré-requisito de segurança. Pode valer já para o JSON atual.
- ✅ `IngestionService`: obter a `psk_identity` da sessão DTLS e resolver o device
  (`list_active` passou a trazer o `device_id`). Credencial fora do cache → `4.01`.
  Validado no QA em 2026-10-05.
- ✅ `IngestionService`: `device_id` do corpo diferente do da PSK → `4.03` (decidido em
  2026-10-05). Validado no QA com o device 12.
- ✅ `IngestionService`: `sensor_id` que não pertença ao device autenticado é descartado
  com log, e a mensagem segue com `2.04` (rejeitar faria o firmware reenviar para sempre).
  Mapa `sensor_id → device_id` por cache com poll, via capability nova `sensor.list_owners`.
  Enquanto o mapa nunca carregou (ex.: `SensorService` fora no startup), a resposta é `5.03`
  para o device guardar os arquivos e reenviar, em vez de descartar tudo. Sensor recém-criado
  só é aceito após o próximo refresh (até 60 s). Validado no QA em 2026-10-05.

### Fase 1 — Contrato e backend ✅
- ✅ Decidir DP1, DP2 e DP3 (2026-10-05).
- ✅ Definir o corpo da resposta CoAP (D11, 2026-10-05).
- ✅ 1a. Decisões F1–F4 e lista de nomes de diagnóstico (D2), 2026-10-05.
- ✅ 1b. `sensor.list_owners` traz também `unit`; o cache do `IngestionService` vira
  `sensor_id → {device_id, unit}` (base para DP1).
- ✅ 1c. Parser SenML JSON como função pura em lib compartilhada, com testes unitários:
  resolve `bn`/`bt`/`t`/`n`/`u`/`v`/`vb`/`fl_`, rejeita campo terminado em `_`
  desconhecido, aceita qualquer número em `v`.
- ✅ 1d. `IngestionService`: despacho por Content-Format (`110` SenML, `50` JSON atual);
  no SenML, prefixo do `bn` contra a PSK (`4.03`), dono do sensor, conversão de unidade
  (DP1), `flags` (F3, migration com `sensor_readings.flags`). Diagnóstico só logado.
  Validado no QA em 2026-10-05 (`V10`; pacote misto com descartes por `fl_`, unidade e
  dono; `Pa` → `kPa`; `bn` de outro device → `4.03`).
- ✅ 1e. Migration `device_telemetry` (D5, sem partição — F2); `IngestionService`
  publica o diagnóstico numa fila `device_telemetry`, agrupado por timestamp; o
  `ReadingWriterService` consome e grava com idempotência. Decidido em 2026-10-05: `seq`
  vai no `data`; conflito em `(device_id, timestamp)` mescla as chaves
  (`data || EXCLUDED.data`); o próprio `ReadingWriterService` consome as duas filas.
  Validado no QA em 2026-10-05 (`V11`; dois instantes viram duas linhas; reenvio não
  duplica; `boot_count` mesclado na linha existente).

### Fase 2 — Simulador e firmware ✅
Decidido em 2026-10-05: o simulador troca totalmente para SenML (S1), com tabela inversa de
unidades por tipo de sensor (S2); no firmware, `seq` em RAM + `boot_count` (F-a), encoder
legado removido (F-b), sem `device_id` provisionado o uplink não envia (F-c). Passos do
firmware detalhados como T2b–T2d em `rdws_weather_node/docs/Plano_Firmware_WeatherNode.md`.

- ✅ `SensorSimulatorService` gerando SenML (2a): `bn`/`bt`/`t`, `seq` por transmissão,
  unidades via `rdws::senml::fromSensorUnit` (inversa de `toSensorUnit`; `%` vira `%RH` em
  humidity e `/` em moisture), diagnóstico sintético (`boot_count`, `rssi`), Content-Format
  110 no `CoapDtlsClient`. Validado no QA em 2026-10-06 (devices 9 e 10).
- ✅ Firmware: encoder SenML JSON com `bt`/`t`, `seq`, `fl_` e diagnóstico (`rssi`,
  `boot_count`, `reset_reason`), unidades SenML (pressão em `Pa`). Validado na L475 em
  2026-10-05/06 (T2c, T2d).
- ✅ Firmware: coleta de `fs_used_pct`, `fs_errors` e evento `fs_reformat`, uma vez por
  transmissão (D9). Feito em 2026-10-07, depois do passo 4 (detalhes no plano do firmware):
  `fs_errors` conta desde o boot, só em RAM; `fs_reformat` fica pendente em `settings` até
  ser entregue; `backlog_count` não foi feito (não há envio parcial). Colunas "Config",
  "FS used (%)", "FS errors" e "Last FS reformat" no painel Device diagnostics.
- ✅ Firmware: `device_id` no `bn` (DP2) provisionado pelo shell (`rdws id set`), sem o
  `CONFIG_RDWS_DEVICE_ID` (T2b). Os `sensor_id` continuam globais (F1) e saem do Kconfig no
  passo 4 (config vinda do backend).
- ✅ Teste ponta-a-ponta: firmware/simulador → `IngestionService` → fila → banco, sem
  duplicação em reenvio (reset no meio do envio, 2026-10-06). Achado no caminho: o módulo
  eswifi sai sempre da porta 5684, e a sessão DTLS órfã após reset prendia o device;
  mitigado com timeout de sessão ociosa de 60 s no `IngestionService`.

### Fase 3 — Observabilidade
Decidido em 2026-10-07:
- **R1**: `last_seen` como coluna `devices.last_seen` (hora de **chegada**, não a da
  leitura), atualizada pelo `ReadingWriterService` no máximo ~1×/min por device (freio em
  memória + guarda no SQL).
- **R2**: alerta de estação silenciosa no Grafana; canal de notificação de frota fica para
  depois (segue nos pontos em aberto).
- **R3**: sem partições por enquanto. O volume é pequeno (~4 mil leituras/dia por estação);
  particionar `sensor_readings` exige trocar a PK (`id` → `(id, timestamp)`) e migrar dados.
  Revisitar quando `sensor_readings` passar de ~50 milhões de linhas ou as consultas do
  dashboard ficarem lentas.
- **R4**: retenção de `device_telemetry` em 90 dias, por um timer diário no
  `ReadingWriterService`.
- **R5**: estação silenciosa = nada chegou há mais de 3 × o intervalo esperado (do
  `transmissions_per_day` da config; `report_interval_s` nos simulados; 3600 s sem config).

- ✅ 3a. `last_seen` (`V14`, coluna + backfill) e view `device_liveness` (intervalo
  esperado e `silent` por device, regra única para painel e alerta); painel "Devices Online
  / Offline" passa a usá-la (antes: `max(timestamp)` das leituras com limite fixo de 10 min).
- ✅ 3b. Regra de frota para estação silenciosa (alerta do Grafana sobre `device_liveness`):
  `infra/grafana/provisioning/alerting/fleet.yml`, pasta "RDWS Fleet", uma instância por
  device (rótulo `device`), dispara após 5 min acima de 3× o intervalo esperado. Devices
  que nunca enviaram nada ficam de fora (instalação pendente, não estação caída). Sem
  contact point ainda (R2). Validada num Grafana 13 local e no QA (2026-10-07).
- ✅ 3c. Painel de atraso de ingestão (`created_at - timestamp`) por device (D10): "Ingestion
  delay", máximo por device em minutos, na linha "Fleet Health" do Farm Overview.
- ✅ 3d. Painéis de saúde da frota no tempo (RSSI, boots, FS por device), complementando a
  tabela Device diagnostics: "RSSI", "Boot count" (degrau = reboot) e "FS used (%)", na
  mesma linha, com os filtros de fazenda/campo/device do dashboard.
- ⬜ 3e. Retenção de `device_telemetry` (90 dias): o `ReadingWriterService` apaga as linhas
  mais antigas que `TELEMETRY_RETENTION_DAYS` (padrão 90) ao iniciar e depois a cada 24 h.
- Partições de `sensor_readings`/`device_telemetry`: adiado (R3).

### Fase 4 — CBOR (quando necessário)
- ⬜ Encoder SenML CBOR no firmware (`zcbor`) e parser no `IngestionService`
  (Content-Format `112`).

## Pontos em aberto

- Partições de `sensor_readings`/`device_telemetry` (adiado na Fase 3, R3).
- Quando desativar o formato JSON legado no `IngestionService`.
- Se vale uma coluna `category` em `sensors` para separar sensores lógicos de saúde
  (bateria/solar) dos ambientais no dashboard do produtor.
- Se o firmware pode esvaziar o backlog parcialmente numa sessão, o que decide se
  `backlog_count` é enviado (D9).
- Limite para considerar uma estação silenciosa (múltiplo fixo do intervalo de
  transmissão ou configurável por device) e onde a regra de frota roda (D10).
- Canal de notificação dos alertas de frota, separado das notificações ao produtor.
