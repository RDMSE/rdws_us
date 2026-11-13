-- Phase 13a (IDOR): devices addressed by a random UUID, like farms (V18) and fields (V19).
-- The station is unaffected: it identifies itself by psk_identity, not by any device id.
ALTER TABLE devices ADD COLUMN public_id UUID NOT NULL DEFAULT gen_random_uuid();
ALTER TABLE devices ADD CONSTRAINT devices_public_id_key UNIQUE (public_id);

-- The device's response carries its field's public id: a copy, kept consistent with field_id
-- by the composite FK (same as fields.farm_public_id in V19).
ALTER TABLE fields ADD CONSTRAINT fields_id_public_id_key UNIQUE (id, public_id);

ALTER TABLE devices ADD COLUMN field_public_id UUID;
-- The UPDATE triggers on devices are per column (OF location, OF is_simulated): not fired here
UPDATE devices SET field_public_id = fields.public_id FROM fields WHERE fields.id = devices.field_id;
ALTER TABLE devices ALTER COLUMN field_public_id SET NOT NULL;
ALTER TABLE devices ADD CONSTRAINT devices_field_public_id_fkey
    FOREIGN KEY (field_id, field_public_id) REFERENCES fields (id, public_id) ON DELETE CASCADE;
CREATE INDEX idx_devices_field_public_id ON devices (field_public_id);
