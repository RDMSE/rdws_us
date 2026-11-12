-- Phase 13a (IDOR): /devices/{id}/config addresses the device by its public id (V20). The
-- config belongs to device_config_service, which reads only its own table, so the row keeps
-- a copy of the device's public id, kept consistent with device_id by the composite FK (same
-- as fields.farm_public_id in V19 and devices.field_public_id in V20).
ALTER TABLE devices ADD CONSTRAINT devices_id_public_id_key UNIQUE (id, public_id);

ALTER TABLE device_configurations ADD COLUMN device_public_id UUID;
-- The UPDATE triggers on device_configurations see no config change here: V12 keeps the
-- version (config unchanged) and V15 records history only on a new version
UPDATE device_configurations
SET device_public_id = devices.public_id
FROM devices WHERE devices.id = device_configurations.device_id;
ALTER TABLE device_configurations ALTER COLUMN device_public_id SET NOT NULL;
ALTER TABLE device_configurations ADD CONSTRAINT device_configurations_device_public_id_fkey
    FOREIGN KEY (device_id, device_public_id) REFERENCES devices (id, public_id) ON DELETE CASCADE;
-- 1:1 with the device (V3), and the lookup key of the API
ALTER TABLE device_configurations
    ADD CONSTRAINT device_configurations_device_public_id_key UNIQUE (device_public_id);

-- The default config created with each device (V3) now carries the copy too
CREATE OR REPLACE FUNCTION create_default_device_config()
RETURNS TRIGGER AS $$
BEGIN
    INSERT INTO device_configurations (device_id, device_public_id, config)
    VALUES (NEW.id, NEW.public_id, '{}'::jsonb);
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;
