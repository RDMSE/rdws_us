-- Phase 13a (IDOR): sensors addressed by a random UUID in the API, like farms, fields and
-- devices (V18-V20). The station protocol keeps the internal sensor id (SenML names
-- "<device>/<sensor>", the config's onboard[].sensor_id), as do sensor_readings.sensor_id.
ALTER TABLE sensors ADD COLUMN public_id UUID NOT NULL DEFAULT gen_random_uuid();
ALTER TABLE sensors ADD CONSTRAINT sensors_public_id_key UNIQUE (public_id);

-- The sensor's response carries its device's public id: a copy, kept consistent with
-- device_id by the composite FK (devices has UNIQUE (id, public_id) since V21).
ALTER TABLE sensors ADD COLUMN device_public_id UUID;
UPDATE sensors SET device_public_id = devices.public_id FROM devices WHERE devices.id = sensors.device_id;
ALTER TABLE sensors ALTER COLUMN device_public_id SET NOT NULL;
ALTER TABLE sensors ADD CONSTRAINT sensors_device_public_id_fkey
    FOREIGN KEY (device_id, device_public_id) REFERENCES devices (id, public_id) ON DELETE CASCADE;
CREATE INDEX idx_sensors_device_public_id ON sensors (device_public_id);
