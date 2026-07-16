-- ─── device_credentials ──────────────────────────────────────────────────────
-- PSK credentials for CoAP/DTLS device authentication (Plano_DeviceCredentials.md).
-- Separate table from device_configurations on purpose: device_config.get is a
-- general-read capability, and mixing an auth secret into that JSONB payload would
-- risk leaking the PSK through the same endpoint that serves ordinary config.
--
-- psk_identity (UUID, not devices.id) is the DTLS handshake hint, so the network
-- layer never leaks a sequential device id (same IDOR concern as Plano_Gateway_HTTP.md
-- Fase 13). psk_key_enc stores nonce || ciphertext || tag (AES-256-GCM), decrypted
-- only in memory at the point of use — never exposed in plaintext after provisioning.

CREATE TABLE device_credentials (
    id           BIGINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
    device_id    BIGINT NOT NULL UNIQUE REFERENCES devices(id) ON DELETE CASCADE,
    psk_identity UUID NOT NULL DEFAULT gen_random_uuid() UNIQUE,
    psk_key_enc  BYTEA NOT NULL,
    status       VARCHAR(16) NOT NULL DEFAULT 'active',
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    rotated_at   TIMESTAMPTZ,
    revoked_at   TIMESTAMPTZ
);
