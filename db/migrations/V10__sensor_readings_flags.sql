-- ─── sensor_readings.flags ──────────────────────────────────────────────────
-- Per-reading flags sent by the station in the SenML `fl_` extension field
-- (Plano_Telemetria.md, DP3/F3), same bitmask as the firmware's RECORD_FLAG_*:
--   0x2 trigger         - this reading fired an edge trigger
--   0x4 partial_window  - aggregation window was incomplete (data quality)
-- 0x1 time_unsynced never reaches the table: IngestionService drops those
-- readings, since their timestamp is meaningless.
-- Existing rows and the legacy JSON payload (no flags) default to 0.

ALTER TABLE sensor_readings
    ADD COLUMN flags SMALLINT NOT NULL DEFAULT 0;
