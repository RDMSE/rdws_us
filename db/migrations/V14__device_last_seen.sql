-- ─── devices.last_seen ───────────────────────────────────────────────────────
-- When the station last talked to the server (arrival time, not the reading's own
-- timestamp: a station catching up on a backlog sends old windows, but it's alive
-- now). Feeds the "silent station" fleet rule and the online/offline panel
-- (Plano_Telemetria.md, D10 / Fase 3, R1).
--
-- Written by ReadingWriterService for every accepted reading or telemetry row,
-- at most about once a minute per device (it skips recent values), so it costs
-- far less than one UPDATE per reading.

ALTER TABLE devices ADD COLUMN last_seen TIMESTAMPTZ;

-- Backfill from what already arrived
UPDATE devices d
SET last_seen = seen.at
FROM (
    SELECT device_id, max(at) AS at
    FROM (
        SELECT s.device_id, max(sr.created_at) AS at
        FROM sensor_readings sr JOIN sensors s ON s.id = sr.sensor_id
        GROUP BY s.device_id
        UNION ALL
        SELECT device_id, max(created_at) FROM device_telemetry GROUP BY device_id
    ) both_sources
    GROUP BY device_id
) seen
WHERE d.id = seen.device_id;

-- ─── device_liveness ────────────────────────────────────────────────────────
-- Per device: last_seen, the interval it's expected to send at, and whether it's
-- silent — no data for more than 3 × that interval (R5). Shared by the online/
-- offline panel and the silent-station fleet alert, so both use one rule.
-- Expected interval, in this order:
--   - transmissions_per_day in the config (real stations: 86400 / value);
--   - report_interval_s (SensorSimulatorService's key);
--   - 3600 s, the firmware's build default (CONFIG_RDWS_UPLINK_INTERVAL_S).
-- A device that never sent anything (last_seen NULL) counts as silent.

CREATE VIEW device_liveness AS
SELECT d.id AS device_id,
       d.last_seen,
       iv.expected_interval_s,
       (d.last_seen IS NULL
        OR d.last_seen < now() - 3 * make_interval(secs => iv.expected_interval_s)) AS silent
FROM devices d
LEFT JOIN device_configurations dc ON dc.device_id = d.id
CROSS JOIN LATERAL (
    SELECT CASE
             WHEN jsonb_typeof(dc.config -> 'transmissions_per_day') = 'number'
                  AND (dc.config ->> 'transmissions_per_day')::numeric > 0
               THEN 86400.0 / (dc.config ->> 'transmissions_per_day')::numeric
             WHEN jsonb_typeof(dc.config -> 'report_interval_s') = 'number'
                  AND (dc.config ->> 'report_interval_s')::numeric > 0
               THEN (dc.config ->> 'report_interval_s')::numeric
             ELSE 3600
           END AS expected_interval_s
) iv;
