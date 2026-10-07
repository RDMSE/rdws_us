-- ─── device_configurations: drop onboard[].source ──────────────────────────
-- A weather station's onboard[] channel is now just the quantity (`chan`) and
-- its sensor_id: which chip measures it is a board detail the firmware knows
-- (decided 2026-10-07; schema in DeviceConfigService). Strips `source` from
-- every stored onboard entry so existing configs stay valid under the new,
-- strict schema. The V12 trigger bumps `version`, so stations pick up the
-- cleaned config on their next uplink.

UPDATE device_configurations
SET config = jsonb_set(
        config, '{onboard}',
        (SELECT coalesce(jsonb_agg(entry - 'source'), '[]'::jsonb)
         FROM jsonb_array_elements(config -> 'onboard') AS entry))
WHERE jsonb_typeof(config -> 'onboard') = 'array'
  AND EXISTS (SELECT 1 FROM jsonb_array_elements(config -> 'onboard') AS entry
              WHERE entry ? 'source');
