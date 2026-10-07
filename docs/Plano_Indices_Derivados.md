Data: 2026-10-07

# Plano — Índices Derivados (temperatura, umidade e pressão)

## Contexto

A estação (`rdws_weather_node`) mede só temperatura, umidade relativa e pressão
(`onboard[]`: `temp`, `humidity`, `press`). Cada grandeza gera um registro `last` por
janela fixa de 10 min alinhada ao relógio UTC, com timestamp = fim da janela
(`rdws_weather_node/docs/Plano_Firmware_WeatherNode.md` §4). No banco as unidades são `°C`, `%` e `kPa`
(`Plano_Telemetria.md`, DP1).

**Pré-requisito de qualidade:** os índices só valem o que valem T e UR. Na bancada o
HTS221 lê ~30 °C por causa do aquecimento da própria placa: isso puxa a UR para baixo, o
que subestima as horas de molhamento, superestima o DPV e atrasa o risco de geada. O Td
sofre menos, porque a pressão de vapor do ar não muda com o aquecimento. Em campo, o
sensor de T/UR fica fora da placa, em abrigo ventilado contra radiação. Perto do limiar de
molhamento (90 %), a incerteza de ±3,5 % de UR do HTS221 pesa: o limiar é parâmetro e pode
precisar de ajuste por sensor.

**Decisão (2026-10-07):** todos os índices derivados são calculados no backend. O
firmware fica só com o edge trigger, que usa limiares sobre as grandezas brutas (sem
correlação entre sinais, `Plano_Ingestion.md`). Motivos:

- Fórmulas e parâmetros mudam sem OTA.
- Vários índices dependem de parâmetros por cultura (Tbase, modelo de frio), que não
  pertencem ao device.
- O histórico pode ser reprocessado quando um método ou parâmetro mudar.

Divisão do cálculo:

- **Instantâneos** (por janela de 10 min): views SQL.
- **Acumulados** (por dia, período ou safra): job que materializa agregados diários no
  fuso local da fazenda (IANA, `farms.timezone`), com views por cultura por cima.
- **Alertas** sobre esses índices: `AlertingService` (`Plano_Alerting.md`, seção
  "Alertas sobre índices derivados").

---

## Classificação dos índices

| Índice | Onde | Entradas | Depende de |
|---|---|---|---|
| Ponto de orvalho (Td) | view instantânea | T, UR | — |
| Déficit de pressão de vapor (DPV) | view instantânea | T, UR | — |
| Delta T (pulverização) | view instantânea | T, UR, P | — |
| Pressão ao nível do mar | view instantânea | P, T | `devices.elevation_m` |
| Tendência barométrica (3 h) | view com janela | P | — |
| Molhamento estimado | job, por período | UR, T | — |
| Tmin/Tmax/Tmédia, UR, horas de frio (Weinberger) | job diário | T, UR | `farms.timezone` |
| ET₀ (Hargreaves e PM com dados faltantes) | job diário | Tmin, Tmax, UR, P | latitude (`devices.location`), `elevation_m` |
| Graus-dia (GDD) acumulados | view por safra sobre o diário | Tmin, Tmax | cultura, Tbase/Tupper, data de início |
| Horas de frio acumuladas | view por safra sobre o diário | T | modelo de frio, início da estação |
| Risco de geada | `AlertingService` | Td, taxa de resfriamento, pôr do sol | latitude |

---

## D1 — Pareamento das grandezas (`weather_samples`)

View base com uma linha por (device, timestamp) e as colunas `t_c`, `rh_pct`, `p_kpa` e
`flags` (OR dos `flags` das três leituras). O join é exato por `timestamp`, porque o
firmware amostra as três grandezas no mesmo fim de janela. Não há interpolação.

- Índice que precisa de duas grandezas usa só as linhas em que ambas existem; canal
  desligado (`enabled: false`) ou leitura perdida resulta em índice nulo naquela janela,
  nunca em valor estimado.
- UR acima de 100 % (sensor saturado ou com condensação) é limitada a 100 para as
  fórmulas; UR ≤ 0 invalida a linha para os índices que dependem dela.
- MVP: um sensor por tipo por `weather_station`, que é a realidade atual. O critério para
  mais de um sensor da mesma grandeza fica em aberto.

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
device_configuration_history     -- conteúdo de cada versão
  device_id  BIGINT (FK → devices.id)
  version    INTEGER
  config     JSONB
  saved_at   TIMESTAMPTZ
  PRIMARY KEY (device_id, version)

device_config_spans              -- quando cada versão valeu no device
  device_id  BIGINT (FK → devices.id)
  version    INTEGER
  first_ts   TIMESTAMPTZ          -- menor timestamp de leitura visto com essa versão
  last_ts    TIMESTAMPTZ          -- maior
  PRIMARY KEY (device_id, version)
```

- `device_configuration_history`: trigger em `device_configurations` grava uma linha a cada
  versão nova (a versão já é controlada pelo banco, V12). Backfill da versão atual na
  migration.
- `device_config_spans`: o `IngestionService` faz upsert a cada pack com `rec_cfg`
  (`Plano_Telemetria.md` D2), com `LEAST`/`GREATEST` nos timestamps de sensor do pack.
- `weather_samples` ganha `cfg_version` e `window_s`: o span que contém o timestamp dá a
  versão, e o histórico dá o `agg_window_s` dela. Amostra fora de qualquer span (dados
  anteriores ao `rec_cfg`, simulador, JSON legado) usa a config atual, e na falta dela o
  default de 600 s.
- Todas as durações deste plano usam `window_s` da amostra, não 600 s fixos: `coverage`
  é a soma de `window_s` sobre a duração do dia, horas = soma de `window_s` das amostras
  que atendem ao critério.
- De quebra, o histórico dá o mapa canal → `sensor_id` de qualquer versão (limitação do
  passo 4e do firmware depois de um boot).

Entregas, separadas: (1) migration das duas tabelas, com trigger e backfill; (2) upsert
de spans no `IngestionService`; (3) `rec_cfg` no encoder do firmware. Só são necessárias
antes da view `weather_samples` (passo 3 da ordem de implementação).

## D2 — Fórmulas instantâneas

Implementadas como funções SQL puras (`IMMUTABLE`), testadas isoladamente com valores de
referência publicados, e usadas pelas views.

- **Pressão de saturação:** Magnus com as constantes de Alduchov e Eskridge (1996):
  `es(T) = 0,61094 · exp(17,625·T / (T + 243,04))` kPa. Pressão de vapor real:
  `ea = es(T) · UR/100`.
- **Ponto de orvalho:** inversa da mesma Magnus, `Td = 243,04·α / (17,625 − α)` com
  `α = ln(UR/100) + 17,625·T/(243,04 + T)`. Abaixo de 0 °C isso é o ponto de orvalho sobre
  água, não o ponto de geada sobre gelo (ver pontos em aberto).
- **DPV do ar:** `DPV = es(T) − ea` kPa. É o DPV do ar; o DPV foliar exigiria a
  temperatura da folha.
- **Delta T:** `ΔT = T − Tw`, com a temperatura de bulbo úmido `Tw` obtida pela equação
  psicrométrica com a pressão medida:
  `ea = es(Tw) − γ·(T − Tw)`, `γ = 0,000662 · P` (kPa/°C, coeficiente de psicrômetro
  ventilado, FAO-56 eq. 15/16). Resolvida por Newton
  (`f'(Tw) = es'(Tw) + γ`), partindo de `Tw = Td`; converge em poucas iterações. A fórmula
  empírica de Stull (2011) não usa pressão (é ajustada para o nível do mar) e serve só como
  referência nos testes perto de 101,3 kPa.
  Faixas usuais de referência para pulverização: abaixo de 2 °C risco de deriva de gotas
  finas e escorrimento; 2–8 °C bom; 8–10 °C atenção; acima de 10 °C evitar. A confirmar
  com agrônomo. Sem vento a indicação é parcial e o dashboard deve dizer isso.
- **Pressão ao nível do mar:**
  `P0 = P · (1 − 0,0065·z / (T + 0,0065·z + 273,15))^−5,257`, com `z = devices.elevation_m`.
  Nula enquanto o device não tiver altitude. Só serve para comparar estações entre si.
- **Tendência barométrica:** `P(t) − P(t − 3 h)` em hPa, por self-join no timestamp
  exato (as janelas são alinhadas a UTC; 3 h precisa ser múltiplo de `window_s`, D1.1). Nula se a leitura de 3 h atrás não existir. Não
  depende de altitude: um viés constante se cancela na diferença. Classes de referência
  (terminologia usual de boletins marinhos), sobre o módulo da variação em intervalos
  semiabertos: estável < 0,1 hPa; lenta [0,1; 1,6); moderada [1,6; 3,6); rápida
  [3,6; 6,0]; muito rápida > 6,0. Em latitude tropical a maré barométrica semidiurna sozinha
  produz variações "lentas" todo dia.

## D3 — Agregado diário (`daily_weather`)

Uma linha por (device, data local), independente de cultura:

```
daily_weather
  device_id      BIGINT  (FK → devices.id)
  local_date     DATE
  timezone       TEXT          -- fuso IANA usado no cálculo
  t_min, t_max, t_mean         NUMERIC  -- t_mean = média das amostras
  rh_min, rh_max, rh_mean      NUMERIC
  p_mean                       NUMERIC
  chill_h                      NUMERIC  -- horas com T < limiar (7,2 °C, Weinberger)
  wet_h                        NUMERIC  -- horas com UR ≥ 90 %
  et0_hs                       NUMERIC  -- Hargreaves-Samani, mm/dia
  et0_pm                       NUMERIC  -- Penman-Monteith com dados faltantes, mm/dia
  samples        INTEGER
  coverage       NUMERIC       -- Σ window_s / duração real do dia local (D1.1)
  computed_at    TIMESTAMPTZ
  PRIMARY KEY (device_id, local_date)
```

- **Limites do dia:** `[meia-noite local, próxima meia-noite local)`, convertidos com
  `AT TIME ZONE farms.timezone`. Como o timestamp é o **fim** da janela, a amostra
  pertence ao dia se `início < ts ≤ fim`: a janela que termina exatamente à meia-noite
  local é do dia anterior.
- **Horário de verão:** o dia local pode ter 23 ou 25 h; a cobertura usa a duração real.
- **Horas** (frio, molhamento) = soma de `window_s` (D1.1) das amostras que atendem ao
  critério. Janela perdida não conta; a cobertura mostra quanto faltou.
- **Tmin/Tmax** saem de amostras pontuais a cada 10 min, não de leitura contínua, e
  subestimam um pouco a amplitude (afeta ET₀ e GDD). Se a diferença importar, o firmware
  pode amostrar T a cada minuto e entregar também `min`/`max` da janela como saídas extras
  (o modelo de `outputs` da config já comporta isso).

### Recálculo com dado atrasado

Com 1 uplink/dia, os dados do dia D chegam ao longo do dia D+1. Uma estação offline por
vários dias entrega o backlog de uma vez. Por isso o job **não fecha o dia uma vez só**:

- Roda periodicamente (ex.: a cada hora).
- **Fila de dias sujos (decisão 2026-10-07):** um trigger `AFTER INSERT` em
  `sensor_readings` grava `(device_id, local_date)` em `dirty_days` (PK nos dois campos,
  `ON CONFLICT DO NOTHING`), com a data local calculada pelo fuso da fazenda e a mesma
  regra de fim de janela dos limites do dia. O job consome a fila: recalcula cada dia
  inteiro, grava por upsert e apaga a linha da fila na mesma transação. O cálculo é
  idempotente.
  - Não usa `sensor_readings.created_at` como marca d'água: `created_at` é `now()`, o
    início da transação. Uma transação que começa antes de o job rodar e faz commit
    depois fica com `created_at` abaixo da marca, e o dia dela nunca seria recalculado.
  - Dia que recebe leitura enquanto o job o recalcula: o trigger grava a linha de novo
    depois de o job apagá-la (ou o job só apaga linhas com `queued_at` ≤ início do
    cálculo), e o dia volta na execução seguinte.
- Mudança de `farms.timezone` exige recálculo completo do histórico daquela fazenda
  (comando manual, não automático).
- A cobertura decide se o dia entra nos acumulados (limiar em aberto).

## D4 — Molhamento por período (`wetness_periods`)

Os modelos de doença fúngica usam a duração de **períodos contínuos** de molhamento e a
temperatura média durante o período. Esses períodos normalmente atravessam a noite, então
a soma diária (`wet_h`) serve para gráfico, mas não para modelo de doença.

```
wetness_periods
  device_id     BIGINT
  started_at    TIMESTAMPTZ
  ended_at      TIMESTAMPTZ NULL   -- NULL = em andamento
  duration_min  INTEGER
  t_mean        NUMERIC
  rh_mean       NUMERIC
```

- Critério: UR ≥ 90 % (parâmetro). Uma janela perdida isolada não interrompe o período;
  duas ou mais seguidas interrompem (parâmetro).
- O recálculo reconstrói os períodos que cruzam o intervalo afetado pelo dado novo,
  estendido até as bordas desses períodos.
- Um sensor de molhamento foliar, se for instalado, substitui o critério por UR.

## D5 — Acumulados por cultura e safra

```
crops
  id, name
  gdd_method       ENUM('avg', 'avg_cutoff')   -- 'single_sine' depois
  t_base           NUMERIC
  t_upper          NUMERIC NULL
  chill_model      ENUM('weinberger')          -- 'utah', 'dynamic' depois
  chill_threshold  NUMERIC DEFAULT 7.2

field_seasons            -- talhão × safra
  id, field_id (FK → fields.id), crop_id (FK → crops.id)
  cultivar         TEXT NULL
  start_date       DATE           -- plantio ou biofix (início do GDD)
  chill_start_date DATE NULL      -- início da contagem de frio
  end_date         DATE NULL
```

- **GDD** por dia, a partir de `daily_weather`:
  - `avg`: `max(0, (Tmax + Tmin)/2 − Tbase)`;
  - `avg_cutoff`: o mesmo com `Tmax` limitada a `Tupper` e `Tmin` elevada a `Tbase`.
  O método é parâmetro da cultura porque as tabelas de fenologia e de pragas são
  calibradas com um método específico; calcular com outro desloca as previsões.
  Acumulado: `SUM(...) OVER (PARTITION BY season ORDER BY local_date)` desde
  `start_date`.
- **Horas de frio (Weinberger):** soma de `chill_h` desde `chill_start_date`. Os modelos
  Utah (unidades com desconto por calor) e Dynamic (porções, com estado intermediário)
  precisam de temperatura horária e, no caso do Dynamic, de processamento sequencial; não
  cabem num `SUM` sobre o diário. Ficam para depois, com uma agregação horária.
- Mudar Tbase, método ou data de início não exige reprocessar nada: os acumulados são
  views sobre `daily_weather`.

## D6 — ET₀

Calculada no job diário. Radiação extraterrestre `Ra` a partir da latitude
(`devices.location`) e do dia do ano (FAO-56, eq. 21–25).

- **Hargreaves-Samani:** `ET₀ = 0,0023 · (Tmédia + 17,8) · √(Tmax − Tmin) · 0,408·Ra`,
  com `Tmédia = (Tmax + Tmin)/2`.
- **Penman-Monteith com dados faltantes** (FAO-56 cap. 3):
  - radiação solar estimada: `Rs = kRs · √(Tmax − Tmin) · Ra`
    (`kRs` ≈ 0,16 interior, 0,19 litoral);
  - `Rso = (0,75 + 2·10⁻⁵·z) · Ra` (usa `elevation_m`; sem altitude, `z = 0`);
  - `Rs/Rso` limitado a 1 no termo de onda longa (`Rnl`, FAO-56 eq. 39). Com `Rs`
    estimado pela amplitude térmica, a razão passa de 1 com frequência, e sem o limite o
    saldo de radiação sai errado;
  - pressão de vapor real a partir da UR medida (FAO-56 eq. 17, com UR máx/mín e
    Tmin/Tmax);
  - `γ` a partir da pressão média medida, em vez da estimada pela altitude;
  - vento default de 2 m/s até existir anemômetro; `G = 0` na escala diária.
- Os dois métodos são gravados. Em clima úmido o PM com dados faltantes costuma errar
  menos que Hargreaves; comparar com uma estação de referência próxima, se houver, antes
  de escolher o que aparece para o produtor.

## D7 — Altitude do device

```
devices.elevation_m       NUMERIC(7,1) NULL
devices.elevation_source  ENUM('dem', 'gps', 'manual') NULL
```

- Preenchida automaticamente a partir de um modelo digital de elevação (Copernicus
  GLO-30 ou SRTM, 30 m) quando `devices.location` é definido ou muda. Pode ir junto do
  trigger que já alimenta `device_location_history`.
- Prioridade: `manual` > `gps` > `dem`. Uma fonte de prioridade menor nunca sobrescreve
  uma de prioridade maior.
- GPS só em variantes cujo modem tenha GNSS (o BC660K-GL não tem). Altitude por GPS é a
  média de dezenas de fixes coletados na instalação ou numa janela de manutenção, com o
  rádio celular parado, enviada com contagem de amostras e VDOP médio.
- Usada pela pressão ao nível do mar e pelo `Rso` do ET₀. A tendência barométrica não
  depende dela.

## D8 — Visualização no Grafana

O stack já existe (Grafana provisionado por arquivo em `infra/grafana/`, datasource
PostgreSQL, dashboard Farm Overview). Os índices entram como uma linha "Agro" no Farm
Overview ou como um dashboard próprio por fazenda.

- **Instantâneos:**
  - Time series de T com Td no mesmo painel (a distância entre as curvas mostra quão
    perto o ar está da saturação).
  - DPV com faixas coloridas (thresholds do painel em modo de área).
  - Delta T com faixas 2/8/10 °C e um painel Stat "Pulverização agora" sobre o último
    valor, com value mappings (Bom / Atenção / Evitar) e a nota "sem vento".
  - Pressão com Stat da tendência de 3 h (classe em texto).
- **Diários:** barras de ET₀ por dia; barras de horas de frio com linha do acumulado; GDD
  acumulado por safra; State timeline para os períodos de molhamento.
- **Fuso:** as linhas de `daily_weather` expõem `time` = meia-noite local convertida
  para `timestamptz`, para as barras caírem no dia certo com o dashboard no fuso da
  fazenda. O fuso do Grafana é por dashboard ou por usuário, não por variável.
- **Períodos longos:** as views instantâneas usam `$__timeFilter`; para meses ou anos,
  `date_bin` com `$__interval` reduz os pontos (Postgres puro, sem TimescaleDB).
- O Grafana atende operação, agrônomo e o MVP. Uma interface própria para o produtor
  (mobile, "dá para pulverizar agora?") fica fora do escopo deste plano.

---

## Pontos em aberto

- Critério do sensor canônico por grandeza quando um device tiver mais de um (ex.:
  `temp` e `temp_lps`): pelo `chan` na config atual (perde histórico se o `sensor_id`
  mudar) ou por uma marca em `sensors`.
- Ponto de geada (sobre gelo) abaixo de 0 °C: usar as constantes de Magnus sobre gelo
  nessa faixa ou manter sobre água e documentar.
- Cobertura mínima para um dia entrar nos acumulados, e o que fazer com dias abaixo dela
  (pular, interpolar, marcar o acumulado como incompleto).
- Onde o job roda: `pg_cron` (só SQL, dentro do banco) ou worker no padrão do
  `ReadingWriterService`. Ele é o primeiro consumidor de um agendador; o recálculo diário
  de `extra_uplinks_utc` (política `sunset_frost`, `rdws_weather_node/docs/Plano_Firmware_WeatherNode.md` §3)
  é o segundo. A manutenção de partições entraria depois, se as partições saírem
  (adiadas, `Plano_Telemetria.md` R3).
- Fonte do modelo de elevação e forma de consulta: raster da região importado no PostGIS
  ou API externa.
- Agregação horária para os modelos de frio Utah e Dynamic.
- Talhão com mais de uma estação: qual alimenta os acumulados da safra (a mais próxima do
  centroide, média, escolha manual).
- `kRs` e vento default por região.
- `min`/`max` de T por janela no firmware, se a subestimação da amplitude importar.
- Faixas agronômicas (DPV por cultura, Delta T): validar com agrônomo e decidir se viram
  parâmetro de `crops`.

## Ordem de implementação

1. ⬜ Migration: `farms.timezone`, `devices.elevation_m`, `devices.elevation_source`.
   - ⬜ 1a. D1.1: `device_configuration_history` + `device_config_spans`; upsert de spans
     no `IngestionService`; `rec_cfg` no encoder do firmware.
2. ⬜ Funções SQL puras (`es`, `dewpoint`, `vpd`, `wet_bulb`, `slp`, `ra`, `et0_hs`,
   `et0_pm`) com testes contra valores de referência (exemplos numéricos da FAO-56; Stull
   2011 para bulbo úmido ao nível do mar).
3. ⬜ View `weather_samples` e views instantâneas (Td, DPV, Delta T, P0, tendência 3 h).
4. ⬜ Painéis instantâneos no Grafana.
5. ⬜ `daily_weather`, fila `dirty_days` (trigger em `sensor_readings`) e job de recálculo.
6. ⬜ `wetness_periods`.
7. ⬜ `crops`, `field_seasons` e views de acumulado (GDD, frio Weinberger).
8. ⬜ Painéis diários no Grafana.
9. ⬜ Alertas sobre os índices (`Plano_Alerting.md`).
