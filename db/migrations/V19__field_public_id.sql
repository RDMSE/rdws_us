-- Phase 13a (IDOR): fields addressed by a random UUID, like farms (V18).
ALTER TABLE fields ADD COLUMN public_id UUID NOT NULL DEFAULT gen_random_uuid();
ALTER TABLE fields ADD CONSTRAINT fields_public_id_key UNIQUE (public_id);

-- A field's response carries its farm's public id. The field service reads only its own
-- table, so the field keeps a copy of it; farms.public_id never changes, so the copy can't
-- go stale. The composite FK makes the copy match farm_id: the pair must exist in farms.
ALTER TABLE farms ADD CONSTRAINT farms_id_public_id_key UNIQUE (id, public_id);

ALTER TABLE fields ADD COLUMN farm_public_id UUID;
UPDATE fields SET farm_public_id = farms.public_id FROM farms WHERE farms.id = fields.farm_id;
ALTER TABLE fields ALTER COLUMN farm_public_id SET NOT NULL;
ALTER TABLE fields ADD CONSTRAINT fields_farm_public_id_fkey
    FOREIGN KEY (farm_id, farm_public_id) REFERENCES farms (id, public_id) ON DELETE CASCADE;
CREATE INDEX idx_fields_farm_public_id ON fields (farm_public_id);
