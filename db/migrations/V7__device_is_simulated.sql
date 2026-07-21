-- ─── devices.is_simulated ────────────────────────────────────────────────────
-- Marks a device as simulated (SensorSimulatorService) vs. real hardware, orthogonal
-- to `type` (weather_station/single_sensor/gateway/other) — see
-- Plano_SensorSimulatorService.md. Immutable after creation: no update path in
-- production is expected to flip this flag, enforced here at the DB level as a
-- safety net regardless of what the application layer allows.

ALTER TABLE devices ADD COLUMN is_simulated BOOLEAN NOT NULL DEFAULT false;

CREATE OR REPLACE FUNCTION prevent_device_is_simulated_change()
RETURNS TRIGGER AS $$
BEGIN
    IF NEW.is_simulated IS DISTINCT FROM OLD.is_simulated THEN
        RAISE EXCEPTION 'devices.is_simulated is immutable after creation';
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_device_is_simulated_immutable
    BEFORE UPDATE OF is_simulated ON devices
    FOR EACH ROW
    EXECUTE FUNCTION prevent_device_is_simulated_change();

CREATE INDEX idx_devices_is_simulated ON devices (is_simulated);
