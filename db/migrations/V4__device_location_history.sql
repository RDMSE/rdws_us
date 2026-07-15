-- ─── device_location_history ────────────────────────────────────────────────
-- Position history for devices. Populated via a trigger on `devices` that only
-- inserts a new row when the displacement from the last known position
-- exceeds a threshold (in meters), filtering out GPS precision noise. The
-- threshold is read from device_configurations.config->>'location_threshold_m',
-- falling back to 10m when absent.

CREATE TABLE device_location_history (
    id          BIGSERIAL PRIMARY KEY,
    device_id   BIGINT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    location    GEOMETRY(POINT, 4326) NOT NULL,
    recorded_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_device_location_history_device_time
    ON device_location_history (device_id, recorded_at DESC);

CREATE INDEX idx_device_location_history_location
    ON device_location_history USING GIST (location);

CREATE OR REPLACE FUNCTION log_device_location_change()
RETURNS TRIGGER AS $$
DECLARE
    threshold_m DOUBLE PRECISION;
BEGIN
    SELECT COALESCE((dc.config->>'location_threshold_m')::DOUBLE PRECISION, 10)
      INTO threshold_m
      FROM device_configurations dc
     WHERE dc.device_id = NEW.id;

    IF OLD.location IS DISTINCT FROM NEW.location
       AND (OLD.location IS NULL
            OR ST_Distance(OLD.location::geography, NEW.location::geography) >= threshold_m)
    THEN
        INSERT INTO device_location_history (device_id, location, recorded_at)
        VALUES (NEW.id, NEW.location, now());
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_device_location_change
    AFTER UPDATE OF location ON devices
    FOR EACH ROW
    EXECUTE FUNCTION log_device_location_change();
