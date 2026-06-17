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
    - type : ENUM('temperature', 'moisture', 'ph', 'humidity', 'luminosity', 'other') NOT NULL
    - unit : VARCHAR(32) NOT NULL (ex: '°C', '%', 'pH')
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

## Retenção de Dados

| Período         | Ação                                      |
|-----------------|-------------------------------------------|
| 0 – 90 dias     | Leituras brutas, acesso frequente          |
| 90 dias – 1 ano | Compressão (TimescaleDB) ou tablespace frio |
| > 1 ano         | Arquivamento ou agregação por hora/dia     |

---

## Observações Técnicas

- **SGBD:** PostgreSQL
    - Extensão **PostGIS** para dados geoespaciais (POINT, POLYGON)
    - Extensão **TimescaleDB** para time-series (substitui particionamento manual)
    - **JSONB** em vez de JSON para `device_configurations` (indexável, mais eficiente)
    - **TIMESTAMPTZ** em todos os campos de data (armazena UTC, essencial para IoT distribuído)
    - Connection pooling via **PgBouncer**
