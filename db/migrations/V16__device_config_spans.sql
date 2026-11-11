-- ─── device_config_spans ────────────────────────────────────────────────────
-- When each config version was in force on a device, as seen in its readings
-- (Plano_Indices_Derivados.md D1.1). The station tags every pack with `rec_cfg`,
-- the config_version in the header of the file the records came from
-- (Plano_Telemetria.md D2); the ReadingWriterService widens the span with the
-- pack's smallest and largest sensor timestamps (LEAST/GREATEST), so redelivered
-- or out-of-order packs change nothing. device_config_history (V15) holds what
-- each version contained; this table says which readings it applied to.
--
-- No FK to device_config_history: a station can still hold files written under a
-- version older than that table's backfill, and losing the span over it would
-- gain nothing.

CREATE TABLE device_config_spans (
    device_id BIGINT      NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    version   INTEGER     NOT NULL,
    first_ts  TIMESTAMPTZ NOT NULL,
    last_ts   TIMESTAMPTZ NOT NULL,
    PRIMARY KEY (device_id, version),
    CHECK (first_ts <= last_ts)
);
