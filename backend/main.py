from contextlib import asynccontextmanager
from datetime import datetime
from typing import Literal
import os

import asyncpg
from fastapi import FastAPI, HTTPException, Path, Query
from fastapi.middleware.cors import CORSMiddleware

pool: asyncpg.Pool | None = None

RESOLUTION_SECONDS = {"1m": 60, "5m": 300, "1h": 3600, "1d": 86400}
OFFLINE_AFTER_SECONDS = 60


@asynccontextmanager
async def lifespan(app: FastAPI):
    global pool
    pool = await asyncpg.create_pool(
        os.getenv("DATABASE_URL", "postgresql://postgres:password@db:5432/maxvolt")
    )
    yield
    await pool.close()


app = FastAPI(title="Maxvolt BMS API", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/health")
async def health():
    return {"status": "ok"}


@app.get("/api/devices")
async def get_devices():
    async with pool.acquire() as conn:
        rows = await conn.fetch(f"""
            SELECT
                d.device_id,
                d.name,
                d.location,
                d.design_capacity_mah,
                d.last_seen,
                d.registered,
                EXTRACT(EPOCH FROM (NOW() - d.last_seen))::integer AS seconds_since_seen,
                CASE WHEN d.last_seen > NOW() - INTERVAL '{OFFLINE_AFTER_SECONDS} seconds'
                     THEN 'online' ELSE 'offline' END AS status,
                r.soc     AS latest_soc,
                r.voltage AS latest_voltage,
                r.status  AS latest_status
            FROM devices d
            LEFT JOIN LATERAL (
                SELECT soc, voltage, status
                FROM bms_readings
                WHERE device_id = d.device_id
                ORDER BY ts DESC
                LIMIT 1
            ) r ON true
            ORDER BY d.device_id
        """)

    return [
        {
            "device_id":            row["device_id"],
            "name":                 row["name"],
            "location":             row["location"],
            "design_capacity_mah":  row["design_capacity_mah"],
            "last_seen":            row["last_seen"].isoformat() if row["last_seen"] else None,
            "registered":           row["registered"].isoformat() if row["registered"] else None,
            "seconds_since_seen":   row["seconds_since_seen"],
            "status":               row["status"],
            "latest_soc":           row["latest_soc"],
            "latest_voltage":       round(row["latest_voltage"], 3) if row["latest_voltage"] else None,
            "latest_status":        row["latest_status"],
        }
        for row in rows
    ]


@app.get("/api/bms/{device_id}")
async def get_bms(
    device_id: str = Path(...),
    start: datetime = Query(...),
    end: datetime = Query(...),
    resolution: Literal["raw", "1m", "5m", "1h", "1d"] = Query("raw"),
):
    if start >= end:
        raise HTTPException(status_code=400, detail="start must be before end")

    async with pool.acquire() as conn:
        exists = await conn.fetchval(
            "SELECT 1 FROM devices WHERE device_id = $1", device_id
        )
        if not exists:
            raise HTTPException(status_code=404, detail=f"device '{device_id}' not found")

        if resolution == "raw":
            rows = await conn.fetch("""
                SELECT
                    ts,
                    voltage, current, power,
                    soc AS soc_avg,
                    soc AS soc_min,
                    soc AS soc_max,
                    soh, remain_mah, full_mah, cycles,
                    temp_mos, temp_bat, cells, status
                FROM bms_readings
                WHERE device_id = $1 AND ts >= $2 AND ts <= $3
                ORDER BY ts ASC
            """, device_id, start, end)
        else:
            interval = float(RESOLUTION_SECONDS[resolution])
            rows = await conn.fetch("""
                SELECT
                    to_timestamp(floor(EXTRACT(EPOCH FROM ts) / $4) * $4) AS ts,
                    AVG(voltage)::real                      AS voltage,
                    AVG(current)::real                      AS current,
                    AVG(power)::real                        AS power,
                    AVG(soc)::real                          AS soc_avg,
                    MIN(soc)                                AS soc_min,
                    MAX(soc)                                AS soc_max,
                    AVG(soh)::real                          AS soh,
                    AVG(remain_mah)::bigint                 AS remain_mah,
                    AVG(full_mah)::bigint                   AS full_mah,
                    AVG(cycles)::bigint                     AS cycles,
                    AVG(temp_mos)::real                     AS temp_mos,
                    ARRAY[
                        AVG(temp_bat[1])::real,
                        AVG(temp_bat[2])::real,
                        AVG(temp_bat[3])::real,
                        AVG(temp_bat[4])::real
                    ]                                       AS temp_bat,
                    (array_agg(cells ORDER BY ts DESC))[1]  AS cells,
                    MODE() WITHIN GROUP (ORDER BY status)   AS status
                FROM bms_readings
                WHERE device_id = $1 AND ts >= $2 AND ts <= $3
                GROUP BY floor(EXTRACT(EPOCH FROM ts) / $4)
                ORDER BY ts ASC
            """, device_id, start, end, interval)

    data = [
        {
            "ts":         row["ts"].isoformat(),
            "voltage":    round(row["voltage"] or 0, 3),
            "current":    round(row["current"] or 0, 3),
            "power":      round(row["power"] or 0, 2),
            "soc_avg":    round(float(row["soc_avg"] or 0), 1),
            "soc_min":    row["soc_min"],
            "soc_max":    row["soc_max"],
            "soh":        round(float(row["soh"] or 0), 1),
            "remain_mah": row["remain_mah"],
            "full_mah":   row["full_mah"],
            "cycles":     row["cycles"],
            "temp_mos":   round(row["temp_mos"] or 0, 1),
            "temp_bat":   [round(t or 0, 1) for t in (row["temp_bat"] or [])],
            "cells":      list(row["cells"] or []),
            "status":     row["status"],
        }
        for row in rows
    ]

    return {
        "device_id":  device_id,
        "resolution": resolution,
        "from":       start.isoformat(),
        "to":         end.isoformat(),
        "count":      len(data),
        "data":       data,
    }
