CREATE DATABASE maxvolt;

\c maxvolt

CREATE TABLE devices (
    device_id           TEXT PRIMARY KEY,
    name                TEXT,
    location            TEXT,
    design_capacity_mah INTEGER,
    last_seen           TIMESTAMPTZ,
    registered          TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE bms_readings (
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
    status      TEXT
);

CREATE INDEX ON bms_readings (device_id, ts DESC);
