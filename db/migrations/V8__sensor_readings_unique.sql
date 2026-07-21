-- ─── sensor_readings idempotency ────────────────────────────────────────────
-- Required by ReadingWriterService (Plano_Ingestion.md): if a queue message is
-- redelivered after a consumer crash, the write must not duplicate the reading.
-- (sensor_id, timestamp) is the natural key — a device can't legitimately report
-- two different values for the same sensor at the same instant.

ALTER TABLE sensor_readings
    ADD CONSTRAINT sensor_readings_sensor_ts_key UNIQUE (sensor_id, timestamp);
