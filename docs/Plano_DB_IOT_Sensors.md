Data 17-06-2026

# Projeto Banco de Dados - Sensor Analysis System

Este arquivo descreve o modelo de dados para o sistema de análise de sensores IoT agrícolas.

## Estrutura de Tabelas

- **farms**
    - id : BIGINT (PK, auto-increment)
    - name : VARCHAR(255) NOT NULL
    - location : POINT (PostGIS)
    - created_at : TIMESTAMPTZ NOT NULL DEFAULT now()
    - updated_at : TIMESTAMPTZ
    - updated_by : VARCHAR(255)

- **fields**
    - id : BIGINT (PK, auto-increment)
    - farm_id : BIGINT (FK → farms.id) NOT NULL
    - name : VARCHAR(255) NOT NULL
    - area : NUMERIC(12, 4) (em hectares)
    - geometry : POLYGON (PostGIS)
    - created_at : TIMESTAMPTZ NOT NULL DEFAULT now()
    - updated_at : TIMESTAMPTZ
    - updated_by : VARCHAR(255)

- **devices**
    - id : BIGINT (PK, auto-increment)
    - field_id : BIGINT (FK → fields.id) NOT NULL
    - type : ENUM('weather_station', 'single_sensor', 'gateway', 'other') NOT NULL
    - installation_date : TIMESTAMPTZ
    - status : ENUM('active', 'inactive', 'maintenance') NOT NULL DEFAULT 'active'
    - location : POINT (PostGIS)
    - created_at : TIMESTAMPTZ NOT NULL DEFAULT now()
    - updated_at : TIMESTAMPTZ
    - updated_by : VARCHAR(255)

- **device_configurations**
    - id : BIGINT (PK, auto-increment)
    - device_id : BIGINT (FK → devices.id) NOT NULL
    - config : JSONB NOT NULL
    - created_at : TIMESTAMPTZ NOT NULL DEFAULT now()
    - updated_at : TIMESTAMPTZ
    - updated_by : VARCHAR(255)

- **sensors**
    - id : BIGINT (PK, auto-increment)
    - device_id : BIGINT (FK → devices.id) NOT NULL
    - type : ENUM('temperature', 'moisture', 'ph', 'humidity', 'luminosity', 'other', 'pressure', 'co2') NOT NULL
        - `pressure` e `co2` adicionados na migration V9 (RDWS-103) — ver "Decisão: tipos `pressure` e `co2`" em Observações Técnicas
    - unit : VARCHAR(32) NOT NULL (ex: '°C', '%', 'pH', 'kPa', 'ppm')
        - Nomenclatura em revisão: o `Plano_Telemetria.md` (DP1) decide se o banco adota as
          unidades SenML (`Cel`, `%RH`, `Pa`) ou se o `IngestionService` converte o payload
          para a nomenclatura atual.
    - location : POINT (PostGIS)
    - created_at : TIMESTAMPTZ NOT NULL DEFAULT now()
    - updated_at : TIMESTAMPTZ
    - updated_by : VARCHAR(255)

- **users**
    - id : BIGINT (PK, auto-increment)
    - username : VARCHAR(255) NOT NULL UNIQUE
    - email : VARCHAR(255) NOT NULL UNIQUE
    - password_hash : VARCHAR(255) NOT NULL
    - role : ENUM('admin', 'operator', 'viewer') NOT NULL DEFAULT 'viewer'
    - active : BOOLEAN NOT NULL DEFAULT true
    - created_at : TIMESTAMPTZ NOT NULL DEFAULT now()
    - updated_at : TIMESTAMPTZ
    - updated_by : VARCHAR(255)

- **sensor_readings** *(append-only — sem update)*
    - id : BIGINT (PK, auto-increment)
    - sensor_id : BIGINT (FK → sensors.id) NOT NULL
    - timestamp : TIMESTAMPTZ NOT NULL
    - value : NUMERIC(12, 6) NOT NULL
    - created_at : TIMESTAMPTZ NOT NULL DEFAULT now()

---

## Correlações

```
Farm (1) ─── (N) Field
Field (1) ─── (N) Device
Device (1) ─── (1) DeviceConfiguration
Device (1) ─── (N) Sensor
Sensor (1) ─── (N) SensorReading
```

> `farm_id` foi removido de `devices` — a fazenda é derivada via `field.farm_id`, evitando inconsistência de dados.

---

## Índices Recomendados

```sql
-- Consultas de leituras por sensor em janela de tempo (mais frequente)
CREATE INDEX idx_sensor_readings_sensor_time ON sensor_readings (sensor_id, timestamp DESC);

-- Consultas geoespaciais
CREATE INDEX idx_devices_location ON devices USING GIST (location);
CREATE INDEX idx_fields_geometry ON fields USING GIST (geometry);

-- Filtros por status de dispositivo
CREATE INDEX idx_devices_status ON devices (status);
```

---

## Particionamento de `sensor_readings`

Leituras podem crescer milhões por dia. Estratégia: particionamento por mês via `RANGE` no PostgreSQL.

```sql
CREATE TABLE sensor_readings (
    ...
) PARTITION BY RANGE (timestamp);

CREATE TABLE sensor_readings_2026_06 PARTITION OF sensor_readings
    FOR VALUES FROM ('2026-06-01') TO ('2026-07-01');
```

> Alternativa recomendada: usar **TimescaleDB** (extensão do PostgreSQL) com `hypertables`. Automatiza o particionamento, adiciona compressão nativa e funções de time-series (`time_bucket`, `last`, `first`) sem overhead de gerenciamento manual.

---

## Telemetria de diagnóstico do device (planejado)

Definida no `Plano_Telemetria.md`; ainda não implementada.

- **device_telemetry** *(append-only, particionada por `timestamp`)* — snapshot de
  diagnóstico por transmissão (RSSI, SNR, `boot_count`, `reset_reason`, uso do FS…).
    - device_id : BIGINT (FK → devices.id) NOT NULL
    - timestamp : TIMESTAMPTZ NOT NULL
    - data : JSONB NOT NULL
    - created_at : TIMESTAMPTZ NOT NULL DEFAULT now()
    - UNIQUE (device_id, timestamp)
- **devices.last_seen** : TIMESTAMPTZ — ou derivado de `max(created_at)` das leituras
  (D10 do `Plano_Telemetria.md`, em aberto).
- Futuro (estação agregadora): **devices.parent_device_id** : BIGINT (FK → devices.id).
- Bateria e painel solar continuam como sensores em `sensor_readings` (D6), por causa do
  alerting.
- O mecanismo de criação/retenção de partições deve ser o mesmo de `sensor_readings`.

---

## Retenção de Dados

| Período         | Ação                                      |
|-----------------|-------------------------------------------|
| 0 – 90 dias     | Leituras brutas, acesso frequente          |
| 90 dias – 1 ano | Compressão (TimescaleDB) ou tablespace frio |
| > 1 ano         | Arquivamento ou agregação por hora/dia     |

`device_telemetry` tem retenção própria, mais curta (30–90 dias, em aberto no
`Plano_Telemetria.md`): diagnóstico não tem valor agronômico de longo prazo.

---

## Observações Técnicas

- **SGBD:** PostgreSQL
    - Extensão **PostGIS** para dados geoespaciais (POINT, POLYGON)
    - Extensão **TimescaleDB** para time-series (substitui particionamento manual)
    - **JSONB** em vez de JSON para `device_configurations` (indexável, mais eficiente)
    - **TIMESTAMPTZ** em todos os campos de data (armazena UTC, essencial para IoT distribuído)
    - Connection pooling via **PgBouncer**

### Decisão: tipos `pressure` e `co2` no `sensor_type` (RDWS-103)

- **Contexto:** o `rdws_thingy_node` exporta pressão atmosférica (LPS22HB) e qualidade do ar como eCO2 em ppm (CCS811, canal `SENSOR_CHAN_CO2` do Zephyr). Sem um tipo dedicado, as duas grandezas eram gravadas como `'other'`, ficando indistinguíveis entre si no mesmo device — e de qualquer sensor futuro realmente não categorizado.
- **Decisão:** adicionar `'pressure'` e `'co2'` ao enum `sensor_type` via `db/migrations/V9__sensor_type_pressure_co2.sql` (`ALTER TYPE ... ADD VALUE`).
- **Unidades esperadas:** `pressure` → `kPa` (unidade do `SENSOR_CHAN_PRESS` no Zephyr; usar `hPa` só se o firmware/loader converter explicitamente); `co2` → `ppm`.
- **Observações:**
    - O CCS811 fornece **eCO2** (CO2 equivalente estimado a partir de VOCs), não CO2 medido por NDIR. O tipo `co2` representa esse valor; se no futuro entrar um sensor de CO2 real ou de TVOC, avaliar um tipo separado (ex.: `tvoc`).
    - `ALTER TYPE ... ADD VALUE` não é reversível no PostgreSQL (não há `DROP VALUE`); remover os tipos exigiria recriar o enum.
    - Os valores adicionados vão para o fim do enum — a ordem do enum não deve ser usada para ordenação semântica.
