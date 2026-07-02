-- Fake data seed for development/testing
-- Run: psql -U <user> -d <dbname> -f db/seeds/seed_fake_data.sql

-- ─── farms ────────────────────────────────────────────────────────────────────

INSERT INTO farms (name, location) VALUES
    ('Fazenda São João',    ST_SetSRID(ST_MakePoint(-47.9292, -15.7801), 4326)),
    ('Fazenda Vista Verde', ST_SetSRID(ST_MakePoint(-48.1023, -16.0145), 4326)),
    ('Fazenda Rio Claro',   ST_SetSRID(ST_MakePoint(-47.5512, -15.5230), 4326));

-- ─── fields ───────────────────────────────────────────────────────────────────

INSERT INTO fields (farm_id, name, area) VALUES
    (1, 'Talhão A', 45.5000),
    (1, 'Talhão B', 32.2500),
    (2, 'Setor Norte', 78.0000),
    (2, 'Setor Sul',   61.7500),
    (3, 'Parcela 01',  20.0000),
    (3, 'Parcela 02',  25.3000);

-- ─── devices ──────────────────────────────────────────────────────────────────

INSERT INTO devices (field_id, type, status, installation_date) VALUES
    (1, 'weather_station', 'active',      now() - interval '6 months'),
    (1, 'single_sensor',   'active',      now() - interval '3 months'),
    (2, 'single_sensor',   'active',      now() - interval '4 months'),
    (3, 'weather_station', 'active',      now() - interval '2 months'),
    (3, 'single_sensor',   'maintenance', now() - interval '5 months'),
    (4, 'single_sensor',   'active',      now() - interval '1 month'),
    (5, 'gateway',         'active',      now() - interval '8 months'),
    (6, 'single_sensor',   'inactive',    now() - interval '1 year');

-- ─── device_configurations ────────────────────────────────────────────────────

INSERT INTO device_configurations (device_id, config) VALUES
    (1, '{"sampling_interval_s": 60, "report_interval_s": 300, "alerts": {"temp_max": 40, "humidity_min": 20}}'),
    (2, '{"sampling_interval_s": 30, "report_interval_s": 120}'),
    (3, '{"sampling_interval_s": 30, "report_interval_s": 120}'),
    (4, '{"sampling_interval_s": 60, "report_interval_s": 300, "alerts": {"temp_max": 38}}'),
    (5, '{"sampling_interval_s": 60, "report_interval_s": 600}'),
    (6, '{"sampling_interval_s": 15, "report_interval_s": 60}'),
    (7, '{"mode": "bridge", "max_devices": 16}'),
    (8, '{"sampling_interval_s": 120, "report_interval_s": 600}');

-- ─── sensors ──────────────────────────────────────────────────────────────────

INSERT INTO sensors (device_id, type, unit) VALUES
    -- device 1 (weather_station) — múltiplos sensores
    (1, 'temperature', '°C'),
    (1, 'humidity',    '%'),
    (1, 'luminosity',  'lux'),
    -- device 2
    (2, 'moisture',    '%'),
    -- device 3
    (3, 'moisture',    '%'),
    (3, 'ph',          'pH'),
    -- device 4 (weather_station)
    (4, 'temperature', '°C'),
    (4, 'humidity',    '%'),
    -- device 5 (em manutenção)
    (5, 'temperature', '°C'),
    -- device 6
    (6, 'moisture',    '%'),
    -- device 8 (inativo)
    (8, 'temperature', '°C');

-- ─── sensor_readings ─────────────────────────────────────────────────────────
-- Gera ~48 leituras por sensor ativo (últimas 24h, a cada 30min)

INSERT INTO sensor_readings (sensor_id, timestamp, value)
SELECT
    s.id,
    now() - (n || ' minutes')::interval,
    CASE s.type
        WHEN 'temperature' THEN round((22 + random() * 12)::numeric, 2)
        WHEN 'humidity'    THEN round((45 + random() * 40)::numeric, 2)
        WHEN 'luminosity'  THEN round((1000 + random() * 80000)::numeric, 0)
        WHEN 'moisture'    THEN round((30 + random() * 50)::numeric, 2)
        WHEN 'ph'          THEN round((5.5 + random() * 2.5)::numeric, 2)
        ELSE round((random() * 100)::numeric, 2)
    END
FROM sensors s
CROSS JOIN generate_series(0, 1410, 30) AS n  -- 0..1410 min (24h), passo 30min
WHERE s.device_id IN (
    SELECT id FROM devices WHERE status = 'active'
);
