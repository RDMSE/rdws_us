-- OTA phase 1 (docs/Plano_OTA.md §3): signed firmware images, and which one each device should run.
-- Owned by firmware_service.

-- The file itself is in a volume, named by its SHA-256; everything here but the variant and the notes
-- was read from it. A withdrawn image is no longer offered; installed copies don't change.
CREATE TABLE firmware_images (
    id           BIGINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
    public_id    UUID        NOT NULL DEFAULT gen_random_uuid() UNIQUE,
    build_id     VARCHAR(16) NOT NULL UNIQUE CHECK (build_id ~ '^[0-9a-f]{16}$'), -- fw_build
    sha256       VARCHAR(64) NOT NULL UNIQUE CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    size_bytes   INTEGER     NOT NULL CHECK (size_bytes > 0),
    board_class  UUID        NOT NULL,                                            -- O7
    version      VARCHAR(64) NOT NULL,
    variant      VARCHAR(16) NOT NULL CHECK (variant IN ('bench', 'field')),      -- O15
    notes        TEXT,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    created_by   VARCHAR(255) NOT NULL,
    withdrawn_at TIMESTAMPTZ,
    withdrawn_by VARCHAR(255)
);

-- 1:1 with the device, created with it. A device takes no update until an admin enables it (O17).
-- Keeps a copy of the device's public id, like device_configurations (V21): firmware_service reads
-- only its own tables.
CREATE TABLE device_firmware (
    device_id        BIGINT PRIMARY KEY,
    device_public_id UUID    NOT NULL UNIQUE,
    ota_enabled      BOOLEAN NOT NULL DEFAULT false,
    ota_changed_at   TIMESTAMPTZ,
    ota_changed_by   VARCHAR(255),
    FOREIGN KEY (device_id, device_public_id) REFERENCES devices (id, public_id) ON DELETE CASCADE
);

INSERT INTO device_firmware (device_id, device_public_id)
SELECT id, public_id FROM devices;

CREATE FUNCTION create_default_device_firmware()
RETURNS TRIGGER AS $$
BEGIN
    INSERT INTO device_firmware (device_id, device_public_id) VALUES (NEW.id, NEW.public_id);
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_create_default_device_firmware
    AFTER INSERT ON devices
    FOR EACH ROW
    EXECUTE FUNCTION create_default_device_firmware();

-- Every target a device ever had; the current one has no replaced_at. An image sent to a station by
-- mistake stays traceable, and so does who allowed a bench one.
CREATE TABLE device_firmware_targets (
    id          BIGINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
    device_id   BIGINT NOT NULL REFERENCES device_firmware (device_id) ON DELETE CASCADE,
    image_id    BIGINT NOT NULL REFERENCES firmware_images (id),
    attempt     INTEGER NOT NULL DEFAULT 1 CHECK (attempt > 0),                   -- O18
    allow_bench BOOLEAN NOT NULL DEFAULT false,                                   -- O15
    set_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    set_by      VARCHAR(255) NOT NULL,
    replaced_at TIMESTAMPTZ,
    replaced_by VARCHAR(255)
);

CREATE UNIQUE INDEX device_firmware_targets_current
    ON device_firmware_targets (device_id) WHERE replaced_at IS NULL;
CREATE INDEX idx_device_firmware_targets_image ON device_firmware_targets (image_id);

-- Replaces the device's current target, or clears it (image NULL), in one statement: the service
-- shares one connection between requests. Locks the device's row, so two admins setting a target at
-- once leave one current.
CREATE FUNCTION set_firmware_target(p_device_id BIGINT, p_image_id BIGINT, p_allow_bench BOOLEAN,
                                    p_by VARCHAR)
RETURNS BIGINT AS $$
DECLARE
    v_id BIGINT;
BEGIN
    PERFORM 1 FROM device_firmware WHERE device_id = p_device_id FOR UPDATE;
    UPDATE device_firmware_targets SET replaced_at = now(), replaced_by = p_by
    WHERE device_id = p_device_id AND replaced_at IS NULL;
    IF p_image_id IS NULL THEN
        RETURN NULL;
    END IF;
    -- Checked again here: withdrawn between the service's check and now
    IF EXISTS (SELECT 1 FROM firmware_images WHERE id = p_image_id AND withdrawn_at IS NOT NULL) THEN
        RAISE EXCEPTION 'image % is withdrawn', p_image_id USING ERRCODE = 'check_violation';
    END IF;
    INSERT INTO device_firmware_targets (device_id, image_id, allow_bench, set_by)
    VALUES (p_device_id, p_image_id, p_allow_bench, p_by)
    RETURNING id INTO v_id;
    RETURN v_id;
END;
$$ LANGUAGE plpgsql;
