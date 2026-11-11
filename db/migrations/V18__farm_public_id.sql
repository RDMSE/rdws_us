-- Phase 13a (IDOR): the API addresses a farm by a random UUID instead of the sequential id,
-- which made every farm enumerable. The internal id stays the primary key and the FK target
-- (fields.farm_id, Grafana joins); public_id never changes, so a child table may copy it.
-- ADD COLUMN with a volatile default fills every existing row with its own UUID.
ALTER TABLE farms ADD COLUMN public_id UUID NOT NULL DEFAULT gen_random_uuid();
ALTER TABLE farms ADD CONSTRAINT farms_public_id_key UNIQUE (public_id);
