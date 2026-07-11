-- device_config is now strictly 1:1 with device (this was already the intention of the model,
-- see Plano_DB_IOT_Sensors.md, but there was nothing to prevent multiple rows per device_id — this
-- schema masked this by only retrieving the most recent one). From now on: an empty default configuration
-- is created automatically alongside the device (trigger), and `device_config.create` ceases to
-- exist as a capability — only GET/UPDATE (merge patch).

-- 1) Deduplicate: keep only the most recent row per device_id, remove the others.
DELETE FROM device_configurations dc
USING device_configurations newer
WHERE dc.device_id = newer.device_id
  AND dc.id < newer.id;

-- 2) Backfill: every device without a config gets an empty default config.
INSERT INTO device_configurations (device_id, config)
SELECT d.id, '{}'::jsonb
FROM devices d
WHERE NOT EXISTS (
    SELECT 1 FROM device_configurations dc WHERE dc.device_id = d.id
);

-- 3) Impor 1:1 de verdade no schema.
ALTER TABLE device_configurations
    ADD CONSTRAINT device_configurations_device_id_key UNIQUE (device_id);

-- 4) Trigger: every insertion into devices creates the default config automatically, in the same
-- transaction — atomic, without relying on coordination between device_service and
-- device_config_service (separate processes).
CREATE OR REPLACE FUNCTION create_default_device_config()
RETURNS TRIGGER AS $$
BEGIN
    INSERT INTO device_configurations (device_id, config)
    VALUES (NEW.id, '{}'::jsonb);
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_create_default_device_config
    AFTER INSERT ON devices
    FOR EACH ROW
    EXECUTE FUNCTION create_default_device_config();
