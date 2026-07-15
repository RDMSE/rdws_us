-- Backfill: ensure every existing device_configurations row has an explicit
-- location_threshold_m, defaulting to 10 (meters) when not already set.

UPDATE device_configurations
SET config = jsonb_set(config, '{location_threshold_m}', '10', true)
WHERE config->>'location_threshold_m' IS NULL;
