Data: 2026-10-07 (reduzido em 2026-11-18, RDWS-221)

# Plano — Índices Derivados: entradas públicas

## Contexto

A estação (`rdws_weather_node`) mede temperatura, umidade relativa e pressão. Índices
derivados delas (ponto de orvalho, déficit de pressão de vapor, agregados diários,
acumulados por cultura) são calculados fora deste repositório, a partir das leituras que a
plataforma guarda. A decisão (2026-10-07) é que o cálculo fica no servidor, não no
firmware: fórmulas e parâmetros mudam sem OTA, e o histórico pode ser reprocessado.

Este documento guarda o que a plataforma fornece para esse cálculo e que foi implementado
aqui: a config com que cada leitura foi produzida (D1.1) e a altitude do device e o fuso da
fazenda (D7). As seções mantêm a numeração original, citada pelas migrations V16 e V17 e
pelo código.

## D1.1 — Config de cada leitura (janela de agregação) (decisão 2026-10-07)

As durações (cobertura, horas de frio e de molhamento, interrupção de período, tendência)
dependem da janela de agregação, e `agg_window_s` muda pela config do device. A config
vigente não basta: `device_configurations` é 1:1, sem histórico, e o device só passa a
usar uma versão nova no fim do ciclo em que ela chegou. O servidor só descobre a troca no
ciclo seguinte, até 24 h depois com 1 uplink/dia.

O firmware já sabe a versão exata de cada registro: o header de cada arquivo guarda o
`config_version` com que ele foi gravado, e o arquivo é selado quando a config muda
(passo 4e do firmware). Desenho:

```
device_config_history            -- conteúdo de cada versão (V15, criada no RDWS-153)
  device_id  BIGINT (FK → devices.id)
  version    INTEGER
  config     JSONB
  changed_at TIMESTAMPTZ
  PRIMARY KEY (device_id, version)

device_config_spans              -- quando cada versão valeu no device
  device_id  BIGINT (FK → devices.id)
  version    INTEGER
  first_ts   TIMESTAMPTZ          -- menor timestamp de leitura visto com essa versão
  last_ts    TIMESTAMPTZ          -- maior
  PRIMARY KEY (device_id, version)
```

- `device_config_history`: trigger em `device_configurations` grava uma linha a cada
  versão nova (a versão já é controlada pelo banco, V12). Backfill da versão atual na
  migration.
- `device_config_spans` (V16): a cada pack com `rec_cfg` (`Plano_Telemetria.md` D2), o
  `IngestionService`, que não acessa o banco, publica o menor e o maior timestamp de sensor
  do pack na fila `device_telemetry` (`type: "config_span"`), e o `ReadingWriterService` faz
  o upsert com `LEAST`/`GREATEST`. Idempotente: reenvio ou ordem trocada não mudam nada.
- Para saber a janela de uma leitura: o span que contém o timestamp dá a versão, e o
  histórico dá o `agg_window_s` dela. Leitura fora de qualquer span (dados anteriores ao
  `rec_cfg`, inclusive os que chegaram pelo antigo JSON legado) fica com a config atual, e na
  falta dela com o default de 600 s. Quem calcula durações a partir das leituras soma o
  `window_s` de cada amostra, não 600 s fixos.
- De quebra, o histórico dá o mapa canal → `sensor_id` de qualquer versão (limitação do
  passo 4e do firmware depois de um boot).
- Desde 2026-11-10 o firmware realinha a amostragem na hora em que `agg_window_s` muda e
  descarta a janela interrompida sem registro (só os sensores lentos existem hoje, com uma
  leitura no fim da janela). Cada troca deixa um buraco de até uma janela antiga, que não
  indica falha da estação. Quando houver
  acumuladores (vento, chuva, passo 5 do firmware), a janela interrompida passa a sair com a
  flag `partial_window`.

Entregas, separadas: (1) ✅ `device_config_history` (V15) e `device_config_spans` (V16);
(2) ✅ spans no `IngestionService` e no `ReadingWriterService` (RDWS-169); (3) ✅ `rec_cfg` no
encoder do firmware. Validado na estação 12 em 2026-11-04: uma troca de `agg_window_s` gerou
dois spans sem sobreposição (v22 até 14:46 UTC, v23 a partir de 14:47 UTC).

## D7 — Altitude do device

```
devices.elevation_m       NUMERIC(7,1) NULL
devices.elevation_source  ENUM('dem', 'gps', 'manual') NULL
```

- Planejado: preenchida automaticamente a partir de um modelo digital de elevação (Copernicus
  GLO-30 ou SRTM, 30 m) quando `devices.location` é definido ou muda. Pode ir junto do
  trigger que já alimenta `device_location_history`.
- Prioridade: `manual` > `gps` > `dem`. Uma fonte de prioridade menor nunca sobrescreve
  uma de prioridade maior.
- GPS só em variantes cujo modem tenha GNSS (o BC660K-GL não tem). Altitude por GPS é a
  média de dezenas de fixes coletados na instalação ou numa janela de manutenção, com o
  rádio celular parado, enviada com contagem de amostras e VDOP médio.
- Entrada dos índices que dependem de altitude (pressão ao nível do mar, radiação de céu
  claro do ET₀).
- ✅ Colunas na V17 (RDWS-182), junto com `farms.timezone`: o "passo 1" da ordem de
  implementação original deste plano, citado na migration. Hoje preenchida pela API como `manual`. O preenchimento
  automático pelo modelo de elevação está no `Backlog.md`.

### Fuso da fazenda

`farms.timezone` (IANA, V17, validado contra `pg_timezone_names`): o dia local de cada
fazenda, para qualquer agregado diário sobre as leituras.
