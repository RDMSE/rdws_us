-- ─── device_liveness while a config change is pending ───────────────────────
-- V14 judged silence against the interval of the *current* config. But a station
-- only gets a new config on its next uplink (D11), and until then it keeps the old
-- schedule: going from 12 h to 2 h right after an uplink 8 h ago marked it silent
-- at once (8 h > 3 × 2 h), though it wasn't due for another 4 h.
--
-- Now, while the version the station last declared (`cfg_version` in
-- device_telemetry) differs from device_configurations.version, the expected
-- interval is the larger of the declared version's and the current one's. Once it
-- declares the current version, only the current interval counts. Devices that
-- never declare `cfg_version` (simulator, old firmware) behave as in V14.

-- ─── config_interval_s ──────────────────────────────────────────────────────
-- Expected uplink interval of a config, in this order (same rule as V14):
--   - transmissions_per_day (real stations: 86400 / value);
--   - report_interval_s (SensorSimulatorService's key);
--   - 3600 s, the firmware's build default (CONFIG_RDWS_UPLINK_INTERVAL_S).
-- STRICT: a NULL config (no such version) gives NULL, which greatest() ignores.

CREATE FUNCTION config_interval_s(config JSONB)
RETURNS NUMERIC AS $$
    SELECT CASE
             WHEN jsonb_typeof(config -> 'transmissions_per_day') = 'number'
                  AND (config ->> 'transmissions_per_day')::numeric > 0
               THEN 86400.0 / (config ->> 'transmissions_per_day')::numeric
             WHEN jsonb_typeof(config -> 'report_interval_s') = 'number'
                  AND (config ->> 'report_interval_s')::numeric > 0
               THEN (config ->> 'report_interval_s')::numeric
             ELSE 3600
           END
$$ LANGUAGE sql IMMUTABLE STRICT;

-- ─── device_config_history ──────────────────────────────────────────────────
-- Every version a device's config went through, so the interval of the version a
-- station still runs is known after the config moved on. Written by trigger, after
-- the version trigger (V12) set NEW.version; an UPDATE that keeps the version adds
-- nothing.

CREATE TABLE device_config_history (
    device_id  BIGINT      NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    version    INTEGER     NOT NULL,
    config     JSONB       NOT NULL,
    changed_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (device_id, version)
);

-- Versions before this migration are gone; only the current one can be recorded.
-- A station still declaring an older one falls back to the current interval (V14).
INSERT INTO device_config_history (device_id, version, config, changed_at)
SELECT device_id, version, config, coalesce(updated_at, created_at)
FROM device_configurations;

CREATE FUNCTION record_device_config_history()
RETURNS TRIGGER AS $$
BEGIN
    IF TG_OP = 'INSERT' OR NEW.version <> OLD.version THEN
        INSERT INTO device_config_history (device_id, version, config)
        VALUES (NEW.device_id, NEW.version, NEW.config)
        ON CONFLICT (device_id, version) DO NOTHING;
    END IF;
    RETURN NULL;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_device_config_history
    AFTER INSERT OR UPDATE ON device_configurations
    FOR EACH ROW
    EXECUTE FUNCTION record_device_config_history();

-- ─── device_liveness ────────────────────────────────────────────────────────
-- Same columns as V14 (expected_interval_s is now the effective one, so the
-- silent-station alert's silence_ratio follows), plus config_pending.
-- The declared version is the latest by the reading's own timestamp: a backlog
-- catching up arrives late but says nothing newer.

CREATE OR REPLACE VIEW device_liveness AS
SELECT d.id AS device_id,
       d.last_seen,
       iv.expected_interval_s,
       (d.last_seen IS NULL
        OR d.last_seen < now() - 3 * make_interval(secs => iv.expected_interval_s)) AS silent,
       coalesce(declared.version <> dc.version, false) AS config_pending
FROM devices d
LEFT JOIN device_configurations dc ON dc.device_id = d.id
LEFT JOIN LATERAL (
    SELECT (t.data ->> 'cfg_version')::integer AS version
    FROM device_telemetry t
    WHERE t.device_id = d.id
      AND jsonb_typeof(t.data -> 'cfg_version') = 'number'
    ORDER BY t.timestamp DESC
    LIMIT 1
) declared ON true
LEFT JOIN device_config_history running
       ON running.device_id = d.id
      AND running.version = declared.version
      AND declared.version <> dc.version
CROSS JOIN LATERAL (
    SELECT greatest(config_interval_s(coalesce(dc.config, '{}'::jsonb)),
                    config_interval_s(running.config)) AS expected_interval_s
) iv;
