-- ─── device_telemetry ───────────────────────────────────────────────────────
-- Device diagnostics as a time series (Plano_Telemetria.md, D5): one row per
-- device per instant, with the SenML diagnostic records of that instant in a
-- JSONB object, e.g. {"seq": 812, "rssi": -67, "boot_count": 3}. Separate from
-- sensor_readings: no `sensors` row per metric, its own retention, and the
-- firmware can add metrics without a migration.
--
-- Not partitioned for now (F2): one row per transmission per device. Partitions
-- and retention come later, with a single mechanism for this table and
-- sensor_readings. UNIQUE (device_id, timestamp) stays valid then, since it
-- includes the partition key.
--
-- Written by ReadingWriterService from the "device_telemetry" queue; a
-- redelivered message or a second pack at the same instant merges keys
-- (data || EXCLUDED.data) instead of duplicating or dropping.

CREATE TABLE device_telemetry (
    device_id  BIGINT      NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    timestamp  TIMESTAMPTZ NOT NULL,
    data       JSONB       NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT device_telemetry_device_ts_key UNIQUE (device_id, timestamp)
);
