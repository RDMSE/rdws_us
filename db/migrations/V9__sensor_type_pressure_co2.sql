-- ─── sensor_type: add pressure, co2 ─────────────────────────────────────────
-- rdws_thingy_node exports pressure (LPS22HB) and eCO2 in ppm (CCS811, Zephyr's
-- SENSOR_CHAN_CO2) as distinct physical quantities - both were being shoehorned
-- into 'other' for lack of a dedicated type, making them indistinguishable from
-- each other (and from any future genuinely-uncategorized sensor) on the same
-- device.

ALTER TYPE sensor_type ADD VALUE 'pressure';
ALTER TYPE sensor_type ADD VALUE 'co2';
