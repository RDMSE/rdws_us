-- ─── farms.timezone, devices.elevation_m / elevation_source ──────────────────
-- Step 1 of Plano_Indices_Derivados.md (RDWS-182). Public columns the derived indices read:
--
-- farms.timezone: IANA name of the farm's local time. Daily aggregates (Tmin/Tmax, chill hours,
-- ET0) are computed per local day (D5). Existing farms get America/Sao_Paulo; the
-- FarmService only accepts names PostgreSQL knows (pg_timezone_names), since a CHECK can't
-- query that view. Changing it later requires recomputing that farm's history (manual, D5).
--
-- devices.elevation_m / elevation_source (D7): the station's altitude, for sea-level pressure and
-- the Rso term of ET0. Source priority manual > gps > dem: a lower one never overwrites a higher
-- one (enforced where it's written). Set through the API it is 'manual'; 'dem' (digital
-- elevation model) and 'gps' come later. Both null, or both set.

ALTER TABLE farms ADD COLUMN timezone TEXT NOT NULL DEFAULT 'America/Sao_Paulo';

CREATE TYPE elevation_source AS ENUM ('dem', 'gps', 'manual');

ALTER TABLE devices
    ADD COLUMN elevation_m      NUMERIC(7, 1)    NULL,
    ADD COLUMN elevation_source elevation_source NULL,
    ADD CONSTRAINT devices_elevation_with_source
        CHECK ((elevation_m IS NULL) = (elevation_source IS NULL));
