CREATE TABLE IF NOT EXISTS bms_readings (
    id          BIGSERIAL    PRIMARY KEY,
    device_id   TEXT         NOT NULL,
    ts          TIMESTAMPTZ  NOT NULL DEFAULT now(),
    voltage     REAL,
    current     REAL,
    power       REAL,
    soc         SMALLINT,
    soh         SMALLINT,
    remain_mah  INTEGER,
    full_mah    INTEGER,
    cycles      INTEGER,
    temp_mos    REAL,
    temp_bat    REAL[],
    cells       INTEGER[],
    charging    BOOLEAN
);

CREATE INDEX IF NOT EXISTS idx_bms_device_ts ON bms_readings (device_id, ts DESC);
