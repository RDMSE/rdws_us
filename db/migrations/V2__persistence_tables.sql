-- V2: Gateway persistence — request history and capability metrics

-- ─── request_history ──────────────────────────────────────────────────────────

CREATE TABLE request_history (
    id          BIGSERIAL    PRIMARY KEY,
    request_id  VARCHAR(64)  NOT NULL UNIQUE,
    capability  VARCHAR(128) NOT NULL,
    service_id  VARCHAR(128),
    success     BOOLEAN      NOT NULL,
    latency_ms  INTEGER      NOT NULL,
    recorded_at TIMESTAMPTZ  NOT NULL DEFAULT now()
);

CREATE INDEX idx_request_history_capability  ON request_history (capability);
CREATE INDEX idx_request_history_recorded_at ON request_history (recorded_at DESC);
CREATE INDEX idx_request_history_service_id  ON request_history (service_id);

-- ─── capability_metrics ───────────────────────────────────────────────────────

CREATE TABLE capability_metrics (
    id              BIGSERIAL    PRIMARY KEY,
    capability      VARCHAR(128) NOT NULL,
    window_start    TIMESTAMPTZ  NOT NULL,
    request_count   BIGINT       NOT NULL DEFAULT 0,
    error_count     BIGINT       NOT NULL DEFAULT 0,
    timeout_count   BIGINT       NOT NULL DEFAULT 0,
    avg_latency_ms  NUMERIC(10,3),
    p99_latency_ms  NUMERIC(10,3),
    min_latency_ms  NUMERIC(10,3),
    max_latency_ms  NUMERIC(10,3),
    recorded_at     TIMESTAMPTZ  NOT NULL DEFAULT now()
);

CREATE INDEX idx_capability_metrics_capability ON capability_metrics (capability);
CREATE INDEX idx_capability_metrics_window     ON capability_metrics (window_start DESC);

-- ─── Cleanup function (called by pg_cron or external job) ─────────────────────

CREATE OR REPLACE FUNCTION cleanup_old_request_history() RETURNS void LANGUAGE sql AS $$
    DELETE FROM request_history WHERE recorded_at < now() - INTERVAL '90 days';
$$;

CREATE OR REPLACE FUNCTION cleanup_old_capability_metrics() RETURNS void LANGUAGE sql AS $$
    DELETE FROM capability_metrics WHERE recorded_at < now() - INTERVAL '30 days';
$$;
