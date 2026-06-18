-- V1: IoT Sensor Analysis System — core schema
-- Extensions

CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS pgcrypto;

-- ─── ENUMs ────────────────────────────────────────────────────────────────────

CREATE TYPE device_type   AS ENUM ('weather_station', 'single_sensor', 'gateway', 'other');
CREATE TYPE device_status AS ENUM ('active', 'inactive', 'maintenance');
CREATE TYPE sensor_type   AS ENUM ('temperature', 'moisture', 'ph', 'humidity', 'luminosity', 'other');
CREATE TYPE user_role     AS ENUM ('admin', 'operator', 'viewer');

-- ─── farms ────────────────────────────────────────────────────────────────────

CREATE TABLE farms (
    id         BIGSERIAL PRIMARY KEY,
    name       VARCHAR(255)  NOT NULL,
    location   GEOMETRY(POINT, 4326),
    created_at TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ,
    updated_by VARCHAR(255)
);

-- ─── fields ───────────────────────────────────────────────────────────────────

CREATE TABLE fields (
    id         BIGSERIAL PRIMARY KEY,
    farm_id    BIGINT        NOT NULL REFERENCES farms(id) ON DELETE CASCADE,
    name       VARCHAR(255)  NOT NULL,
    area       NUMERIC(12,4),
    geometry   GEOMETRY(POLYGON, 4326),
    created_at TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ,
    updated_by VARCHAR(255)
);

-- ─── devices ──────────────────────────────────────────────────────────────────

CREATE TABLE devices (
    id                BIGSERIAL PRIMARY KEY,
    field_id          BIGINT        NOT NULL REFERENCES fields(id) ON DELETE CASCADE,
    type              device_type   NOT NULL,
    status            device_status NOT NULL DEFAULT 'active',
    installation_date TIMESTAMPTZ,
    location          GEOMETRY(POINT, 4326),
    created_at        TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at        TIMESTAMPTZ,
    updated_by        VARCHAR(255)
);

-- ─── device_configurations ────────────────────────────────────────────────────

CREATE TABLE device_configurations (
    id         BIGSERIAL PRIMARY KEY,
    device_id  BIGINT      NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    config     JSONB       NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ,
    updated_by VARCHAR(255)
);

-- ─── sensors ──────────────────────────────────────────────────────────────────

CREATE TABLE sensors (
    id         BIGSERIAL PRIMARY KEY,
    device_id  BIGINT      NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    type       sensor_type NOT NULL,
    unit       VARCHAR(32) NOT NULL,
    location   GEOMETRY(POINT, 4326),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ,
    updated_by VARCHAR(255)
);

-- ─── sensor_readings (append-only) ────────────────────────────────────────────

CREATE TABLE sensor_readings (
    id         BIGSERIAL    PRIMARY KEY,
    sensor_id  BIGINT       NOT NULL REFERENCES sensors(id) ON DELETE CASCADE,
    timestamp  TIMESTAMPTZ  NOT NULL,
    value      NUMERIC(12,6) NOT NULL,
    created_at TIMESTAMPTZ  NOT NULL DEFAULT now()
);

-- ─── users ────────────────────────────────────────────────────────────────────

CREATE TABLE users (
    id            BIGSERIAL    PRIMARY KEY,
    username      VARCHAR(255) NOT NULL UNIQUE,
    email         VARCHAR(255) NOT NULL UNIQUE,
    password_hash VARCHAR(255) NOT NULL,
    role          user_role    NOT NULL DEFAULT 'viewer',
    active        BOOLEAN      NOT NULL DEFAULT true,
    created_at    TIMESTAMPTZ  NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ,
    updated_by    VARCHAR(255)
);

-- ─── Indexes ──────────────────────────────────────────────────────────────────

CREATE INDEX idx_sensor_readings_sensor_time ON sensor_readings (sensor_id, timestamp DESC);

CREATE INDEX idx_devices_location  ON devices USING GIST (location);
CREATE INDEX idx_fields_geometry   ON fields  USING GIST (geometry);
CREATE INDEX idx_farms_location    ON farms   USING GIST (location);
CREATE INDEX idx_sensors_location  ON sensors USING GIST (location);

CREATE INDEX idx_devices_status    ON devices (status);
CREATE INDEX idx_devices_field_id  ON devices (field_id);
CREATE INDEX idx_fields_farm_id    ON fields  (farm_id);
CREATE INDEX idx_sensors_device_id ON sensors (device_id);
CREATE INDEX idx_sensor_readings_ts ON sensor_readings (timestamp DESC);

-- ─── Seed: default admin user ──────────────────────────────────────────────────
-- password: 'changeme' — MUST be rotated before any production use

INSERT INTO users (username, email, password_hash, role)
VALUES (
    'admin',
    'admin@localhost',
    crypt('changeme', gen_salt('bf', 12)),
    'admin'
);
