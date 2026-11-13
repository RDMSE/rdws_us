-- Fake data seed for development/testing
-- Run: psql -U <user> -d <dbname> -f db/seeds/seed_fake_data.sql

-- ─── farms ────────────────────────────────────────────────────────────────────

INSERT INTO farms (name, location) VALUES
    ('Fazenda São João',    ST_SetSRID(ST_MakePoint(-47.9292, -15.7801), 4326)),
    ('Fazenda Vista Verde', ST_SetSRID(ST_MakePoint(-48.1023, -16.0145), 4326)),
    ('Fazenda Rio Claro',   ST_SetSRID(ST_MakePoint(-47.5512, -15.5230), 4326));

-- ─── fields ───────────────────────────────────────────────────────────────────

-- Each child row also stores its parent's public id (V19-V22), copied from the parent here
INSERT INTO fields (farm_id, farm_public_id, name, area)
SELECT v.farm_id, f.public_id, v.name, v.area
FROM (VALUES
    (1, 1, 'Talhão A', 45.5000),
    (2, 1, 'Talhão B', 32.2500),
    (3, 2, 'Setor Norte', 78.0000),
    (4, 2, 'Setor Sul',   61.7500),
    (5, 3, 'Parcela 01',  20.0000),
    (6, 3, 'Parcela 02',  25.3000)
) AS v(n, farm_id, name, area)
JOIN farms f ON f.id = v.farm_id
ORDER BY v.n; -- ids in the listed order

-- ─── devices ──────────────────────────────────────────────────────────────────

INSERT INTO devices (field_id, field_public_id, type, status, installation_date)
SELECT v.field_id, f.public_id, v.type::device_type, v.status::device_status, v.installed
FROM (VALUES
    (1, 1, 'weather_station', 'active',      now() - interval '6 months'),
    (2, 1, 'single_sensor',   'active',      now() - interval '3 months'),
    (3, 2, 'single_sensor',   'active',      now() - interval '4 months'),
    (4, 3, 'weather_station', 'active',      now() - interval '2 months'),
    (5, 3, 'single_sensor',   'maintenance', now() - interval '5 months'),
    (6, 4, 'single_sensor',   'active',      now() - interval '1 month'),
    (7, 5, 'gateway',         'active',      now() - interval '8 months'),
    (8, 6, 'single_sensor',   'inactive',    now() - interval '1 year')
) AS v(n, field_id, type, status, installed)
JOIN fields f ON f.id = v.field_id
ORDER BY v.n; -- ids in the listed order: the configs below rely on them

-- ─── device_configurations ────────────────────────────────────────────────────

-- device_configurations already exists (empty, with the device's public id) for every device via
-- the trigger (V3, V21): only the config is set here.
UPDATE device_configurations dc SET config = v.config::jsonb
FROM (VALUES
    (1, '{"sampling_interval_s": 60, "report_interval_s": 300, "alerts": {"temp_max": 40, "humidity_min": 20}}'),
    (2, '{"sampling_interval_s": 30, "report_interval_s": 120}'),
    (3, '{"sampling_interval_s": 30, "report_interval_s": 120}'),
    (4, '{"sampling_interval_s": 60, "report_interval_s": 300, "alerts": {"temp_max": 38}}'),
    (5, '{"sampling_interval_s": 60, "report_interval_s": 600}'),
    (6, '{"sampling_interval_s": 15, "report_interval_s": 60}'),
    (7, '{"mode": "bridge", "max_devices": 16}'),
    (8, '{"sampling_interval_s": 120, "report_interval_s": 600}')
) AS v(device_id, config)
WHERE dc.device_id = v.device_id;

-- ─── sensors ──────────────────────────────────────────────────────────────────

INSERT INTO sensors (device_id, device_public_id, type, unit)
SELECT v.device_id, d.public_id, v.type::sensor_type, v.unit
FROM (VALUES
    -- device 1 (weather_station) — multiple sensors
    (1, 1, 'temperature', '°C'),
    (2, 1, 'humidity',    '%'),
    (3, 1, 'luminosity',  'lux'),
    -- device 2
    (4, 2, 'moisture',    '%'),
    -- device 3
    (5, 3, 'moisture',    '%'),
    (6, 3, 'ph',          'pH'),
    -- device 4 (weather_station)
    (7, 4, 'temperature', '°C'),
    (8, 4, 'humidity',    '%'),
    -- device 5 (under maintenance)
    (9, 5, 'temperature', '°C'),
    -- device 6
    (10, 6, 'moisture',    '%'),
    -- device 8 (inactive)
    (11, 8, 'temperature', '°C')
) AS v(n, device_id, type, unit)
JOIN devices d ON d.id = v.device_id
ORDER BY v.n;

-- ─── sensor_readings ─────────────────────────────────────────────────────────
-- Generates ~48 readings per active sensor (last 24h, every 30min)

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
