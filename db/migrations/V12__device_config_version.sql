-- ─── device_configurations.version ──────────────────────────────────────────
-- Version of the device's configuration, owned by the database: a station
-- declares the version it runs (SenML diagnostic `cfg_version`) and the
-- IngestionService answers with the full config when it's behind
-- (Plano_Telemetria.md, D11; Plano_Firmware_WeatherNode.md step 4).
--
-- Starts at 1 and goes up by one on every actual change of `config` — an
-- UPDATE that leaves `config` as is (e.g. an empty merge patch) keeps it, and
-- no client can set it: the trigger overrides whatever the statement wrote.

ALTER TABLE device_configurations ADD COLUMN version INTEGER NOT NULL DEFAULT 1;

CREATE OR REPLACE FUNCTION set_device_config_version()
RETURNS TRIGGER AS $$
BEGIN
    IF TG_OP = 'INSERT' THEN
        NEW.version := 1;
    ELSIF NEW.config IS DISTINCT FROM OLD.config THEN
        NEW.version := OLD.version + 1;
    ELSE
        NEW.version := OLD.version;
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_device_config_version
    BEFORE INSERT OR UPDATE ON device_configurations
    FOR EACH ROW
    EXECUTE FUNCTION set_device_config_version();
