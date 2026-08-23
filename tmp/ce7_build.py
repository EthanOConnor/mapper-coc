#!/usr/bin/env python3
"""Build a terrain-aware illumination chart for the CE-7 planning point.

This script is temporary CI machinery. It downloads public NASA terrain and
NAIF ephemeris data, computes a terrain horizon, and emits a documented package.
"""

from __future__ import annotations

import csv
import gzip
import hashlib
import json
import math
import os
import shutil
import sys
import time
import urllib.request
import zipfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import rasterio
from pyproj import Transformer
from rasterio.windows import Window, from_bounds
import spiceypy as spice


OUT = Path("ce7_output")
CACHE = Path("ce7_cache")
OUT.mkdir(exist_ok=True)
CACHE.mkdir(exist_ok=True)

SITE_LAT_DEG = -88.8
SITE_LON_E_DEG = 123.4
OBSERVER_HEIGHT_M = 2.0
MOON_RADIUS_M = 1_737_400.0
AZ_STEP_DEG = 0.2
AZ_BINS = int(round(360.0 / AZ_STEP_DEG))
FINE_RADIUS_M = 20_000.0
REGIONAL_RADIUS_M = 180_000.0
SUN_RADIUS_KM = 695_700.0
START_UTC = "2026-01-01T00:00:00"
END_UTC_EXCLUSIVE = "2031-01-01T00:00:00"
STEP_SECONDS = 600

FINE_URLS = {
    "Site01 5 m": "https://pgda.gsfc.nasa.gov/data/LOLA_5mpp/Site01/Site01_final_adj_5mpp_surf.tif",
    "Site04 5 m": "https://pgda.gsfc.nasa.gov/data/LOLA_5mpp/Site04/Site04_final_adj_5mpp_surf.tif",
    "Site06 5 m": "https://pgda.gsfc.nasa.gov/data/LOLA_5mpp/Site06/Site06_final_adj_5mpp_surf.tif",
    "Site07 5 m": "https://pgda.gsfc.nasa.gov/data/LOLA_5mpp/Site07/Site07_final_adj_5mpp_surf.tif",
    "Site11 5 m": "https://pgda.gsfc.nasa.gov/data/LOLA_5mpp/Site11/Site11_final_adj_5mpp_surf.tif",
    "Site20 5 m": "https://pgda.gsfc.nasa.gov/data/LOLA_5mpp/Site20/Site20_final_adj_5mpp_surf.tif",
    "Site23 5 m": "https://pgda.gsfc.nasa.gov/data/LOLA_5mpp/Site23/Site23_final_adj_5mpp_surf.tif",
    "Site42 5 m": "https://pgda.gsfc.nasa.gov/data/LOLA_5mpp/Site42/Site42_final_adj_5mpp_surf.tif",
}

REGIONAL_URLS = {
    "LOLA 80 m south-polar adjusted": "https://pgda.gsfc.nasa.gov/data/LOLA_20mpp/LDEM_80S_80MPP_ADJ.TIF",
    "LOLA 80 m south-polar": "https://pgda.gsfc.nasa.gov/data/LOLA_20mpp/LDEM_80S_80MPP.TIF",
    "LOLA 10 m south of 83S": "https://pgda.gsfc.nasa.gov/data/LOLA_20mpp/LDEM_83S_10MPP_ADJ.TIF",
    "LOLA 5 m south of 87S": "https://pgda.gsfc.nasa.gov/data/LOLA_5mpp/87S/ldem_87s_5mpp.tif",
}

KERNEL_URLS = {
    "naif0012.tls": "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/lsk/naif0012.tls",
    "de421.bsp": "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/spk/planets/de421.bsp",
    "pck00010.tpc": "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/pck/pck00010.tpc",
    "moon_pa_de421_1900-2050.bpc": "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/pck/moon_pa_de421_1900-2050.bpc",
    "moon_080317.tf": "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/fk/satellites/moon_080317.tf",
}

GDAL_ENV = {
    "GDAL_DISABLE_READDIR_ON_OPEN": "EMPTY_DIR",
    "CPL_VSIL_CURL_ALLOWED_EXTENSIONS": ".tif,.TIF",
    "GDAL_HTTP_MULTIRANGE": "YES",
    "GDAL_HTTP_MERGE_CONSECUTIVE_RANGES": "YES",
    "GDAL_HTTP_RETRY_DELAY": "2",
    "GDAL_HTTP_MAX_RETRY": "4",
}


@dataclass
class TerrainSource:
    label: str
    url: str
    crs: str
    bounds: list[float]
    width: int
    height: int
    resolution_m: float
    site_x: float
    site_y: float
    site_raw: float
    site_elev_m: float


def log(message: str) -> None:
    print(message, flush=True)


def download(url: str, destination: Path, retries: int = 4) -> Path:
    if destination.exists() and destination.stat().st_size > 0:
        return destination
    temp = destination.with_suffix(destination.suffix + ".part")
    for attempt in range(1, retries + 1):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "CE7-terrain-illumination/1.0"})
            with urllib.request.urlopen(req, timeout=120) as response, temp.open("wb") as fh:
                shutil.copyfileobj(response, fh, length=1024 * 1024)
            temp.replace(destination)
            return destination
        except Exception as exc:
            if temp.exists():
                temp.unlink()
            if attempt == retries:
                raise
            log(f"Download retry {attempt}/{retries} for {url}: {exc}")
            time.sleep(2 * attempt)
    raise RuntimeError("unreachable")


def raw_to_elevation(values: np.ndarray | float) -> np.ndarray | float:
    arr = np.asarray(values, dtype=np.float64)
    finite = arr[np.isfinite(arr)]
    if finite.size == 0:
        return arr
    median = float(np.median(finite))
    if median > 1_000_000.0:
        arr = arr - MOON_RADIUS_M
    if np.isscalar(values):
        return float(arr)
    return arr


def inspect_source(label: str, url: str) -> TerrainSource | None:
    path = "/vsicurl/" + url
    try:
        with rasterio.Env(**GDAL_ENV):
            with rasterio.open(path) as src:
                if src.crs is None:
                    return None
                to_map = Transformer.from_crs(src.crs.geodetic_crs, src.crs, always_xy=True)
                x0, y0 = to_map.transform(SITE_LON_E_DEG, SITE_LAT_DEG)
                b = src.bounds
                if not (b.left <= x0 <= b.right and b.bottom <= y0 <= b.top):
                    return None
                raw = float(next(src.sample([(x0, y0)]))[0])
                if src.nodata is not None and math.isclose(raw, float(src.nodata), rel_tol=0, abs_tol=1e-6):
                    return None
                elev = float(raw_to_elevation(raw))
                resolution = float(max(abs(src.transform.a), abs(src.transform.e)))
                return TerrainSource(
                    label=label,
                    url=url,
                    crs=src.crs.to_string(),
                    bounds=[float(v) for v in b],
                    width=int(src.width),
                    height=int(src.height),
                    resolution_m=resolution,
                    site_x=float(x0),
                    site_y=float(y0),
                    site_raw=raw,
                    site_elev_m=elev,
                )
    except Exception as exc:
        log(f"Terrain source unavailable: {label}: {exc}")
        return None


def choose_source(candidates: dict[str, str], max_resolution_m: float | None = None) -> TerrainSource | None:
    matches: list[TerrainSource] = []
    for label, url in candidates.items():
        source = inspect_source(label, url)
        if source is not None:
            if max_resolution_m is None or source.resolution_m <= max_resolution_m:
                matches.append(source)
                log(f"Source covers site: {source.label}; nominal pixel {source.resolution_m:.3f} m")
    if not matches:
        return None
    matches.sort(key=lambda s: s.resolution_m)
    return matches[0]


def local_projection_basis(src: rasterio.DatasetReader, x0: float, y0: float) -> tuple[np.ndarray, np.ndarray, float]:
    to_map = Transformer.from_crs(src.crs.geodetic_crs, src.crs, always_xy=True)
    eps = 1e-3
    xn, yn = to_map.transform(SITE_LON_E_DEG, SITE_LAT_DEG + eps)
    xe, ye = to_map.transform(SITE_LON_E_DEG + eps, SITE_LAT_DEG)
    n_vec = np.array([xn - x0, yn - y0], dtype=np.float64)
    e_vec = np.array([xe - x0, ye - y0], dtype=np.float64)
    north_true_m = MOON_RADIUS_M * math.radians(eps)
    east_true_m = MOON_RADIUS_M * max(math.cos(math.radians(SITE_LAT_DEG)), 1e-9) * math.radians(eps)
    scale_n = np.linalg.norm(n_vec) / north_true_m
    scale_e = np.linalg.norm(e_vec) / east_true_m
    n_unit = n_vec / np.linalg.norm(n_vec)
    e_vec = e_vec - n_unit * float(np.dot(e_vec, n_unit))
    e_unit = e_vec / np.linalg.norm(e_vec)
    scale = float((scale_n + scale_e) / 2.0)
    return e_unit, n_unit, scale


def clip_window(window: Window, width: int, height: int) -> Window:
    col_off = max(0, int(math.floor(window.col_off)))
    row_off = max(0, int(math.floor(window.row_off)))
    col_end = min(width, int(math.ceil(window.col_off + window.width)))
    row_end = min(height, int(math.ceil(window.row_off + window.height)))
    return Window(col_off, row_off, max(0, col_end - col_off), max(0, row_end - row_off))


def accumulate_horizon(
    source: TerrainSource,
    observer_elev_m: float,
    max_radius_m: float,
    min_radius_m: float,
    vertical_shift_m: float,
    horizon: np.ndarray,
    chunk: int = 768,
) -> dict[str, float | int | str]:
    path = "/vsicurl/" + source.url
    accepted = 0
    visited = 0
    max_seen = -90.0
    with rasterio.Env(**GDAL_ENV):
        with rasterio.open(path) as src:
            e_unit, n_unit, map_scale = local_projection_basis(src, source.site_x, source.site_y)
            radius_map = max_radius_m * map_scale
            overall = clip_window(
                from_bounds(
                    source.site_x - radius_map,
                    source.site_y - radius_map,
                    source.site_x + radius_map,
                    source.site_y + radius_map,
                    transform=src.transform,
                ),
                src.width,
                src.height,
            )
            if overall.width <= 0 or overall.height <= 0:
                raise RuntimeError(f"Empty terrain window for {source.label}")

            row_start = int(overall.row_off)
            row_stop = int(overall.row_off + overall.height)
            col_start = int(overall.col_off)
            col_stop = int(overall.col_off + overall.width)

            for row_off in range(row_start, row_stop, chunk):
                h = min(chunk, row_stop - row_off)
                for col_off in range(col_start, col_stop, chunk):
                    w = min(chunk, col_stop - col_off)
                    win = Window(col_off, row_off, w, h)
                    arr = src.read(1, window=win, masked=True)
                    if arr.count() == 0:
                        continue
                    rows = row_off + np.arange(h, dtype=np.float64) + 0.5
                    cols = col_off + np.arange(w, dtype=np.float64) + 0.5
                    xs = src.transform.c + cols * src.transform.a
                    ys = src.transform.f + rows * src.transform.e
                    dx = xs[None, :] - source.site_x
                    dy = ys[:, None] - source.site_y
                    east_map = dx * e_unit[0] + dy * e_unit[1]
                    north_map = dx * n_unit[0] + dy * n_unit[1]
                    dist_surface = np.hypot(east_map, north_map) / map_scale
                    valid = (~np.ma.getmaskarray(arr)) & np.isfinite(np.asarray(arr))
                    valid &= dist_surface >= min_radius_m
                    valid &= dist_surface <= max_radius_m
                    if not np.any(valid):
                        continue
                    z = raw_to_elevation(np.asarray(arr, dtype=np.float64)) + vertical_shift_m
                    d = dist_surface[valid]
                    gamma = d / MOON_RADIUS_M
                    target_r = MOON_RADIUS_M + z[valid]
                    observer_r = MOON_RADIUS_M + observer_elev_m + OBSERVER_HEIGHT_M
                    vertical = target_r * np.cos(gamma) - observer_r
                    horizontal = target_r * np.sin(gamma)
                    angle = np.degrees(np.arctan2(vertical, horizontal))
                    az = np.degrees(np.arctan2(east_map[valid], north_map[valid])) % 360.0
                    bins = np.floor(az / AZ_STEP_DEG).astype(np.int64) % AZ_BINS
                    np.maximum.at(horizon, bins, angle)
                    accepted += int(angle.size)
                    visited += int(w * h)
                    max_seen = max(max_seen, float(np.nanmax(angle)))
    return {
        "label": source.label,
        "url": source.url,
        "resolution_m": source.resolution_m,
        "max_radius_m": max_radius_m,
        "min_radius_m": min_radius_m,
        "pixels_accepted": accepted,
        "pixels_visited": visited,
        "max_horizon_angle_deg": max_seen,
    }


def fill_circular(values: np.ndarray) -> np.ndarray:
    values = values.astype(np.float64, copy=True)
    good = np.isfinite(values) & (values > -89.0)
    if not np.any(good):
        return values
    x = np.arange(values.size)
    gx = x[good]
    gy = values[good]
    xp = np.concatenate([gx - values.size, gx, gx + values.size])
    yp = np.concatenate([gy, gy, gy])
    return np.interp(x, xp, yp)


def compute_horizon() -> tuple[np.ndarray, dict, TerrainSource, TerrainSource | None]:
    fine = choose_source(FINE_URLS, max_resolution_m=15.0)
    if fine is None:
        raise RuntimeError("No public 5 m site DEM covered the modeled point")
    regional = choose_source(REGIONAL_URLS)
    observer_elev = fine.site_elev_m
    smooth_dip = -math.degrees(math.acos(MOON_RADIUS_M / (MOON_RADIUS_M + OBSERVER_HEIGHT_M)))
    horizon = np.full(AZ_BINS, smooth_dip, dtype=np.float64)
    diagnostics: dict = {
        "site": {
            "latitude_deg": SITE_LAT_DEG,
            "longitude_east_deg": SITE_LON_E_DEG,
            "observer_height_m": OBSERVER_HEIGHT_M,
            "fine_dem_site_elevation_m": observer_elev,
            "smooth_sphere_horizon_deg": smooth_dip,
        },
        "sources": [],
    }

    diagnostics["sources"].append(
        accumulate_horizon(
            fine,
            observer_elev_m=observer_elev,
            max_radius_m=FINE_RADIUS_M,
            min_radius_m=max(7.5, fine.resolution_m * 1.5),
            vertical_shift_m=0.0,
            horizon=horizon,
        )
    )

    if regional is not None:
        vertical_shift = observer_elev - regional.site_elev_m
        diagnostics["regional_vertical_alignment_shift_m"] = vertical_shift
        max_radius = REGIONAL_RADIUS_M
        # Avoid attempting a gigantic 5/10 m regional crop if an 80 m product is unavailable.
        if regional.resolution_m < 30.0:
            max_radius = 90_000.0
        diagnostics["sources"].append(
            accumulate_horizon(
                regional,
                observer_elev_m=observer_elev,
                max_radius_m=max_radius,
                min_radius_m=max(FINE_RADIUS_M * 0.8, regional.resolution_m * 1.5),
                vertical_shift_m=vertical_shift,
                horizon=horizon,
            )
        )
    else:
        diagnostics["regional_warning"] = "No regional DEM endpoint was readable; horizon is limited to the fine DEM radius."

    horizon = fill_circular(horizon)
    diagnostics["horizon"] = {
        "azimuth_step_deg": AZ_STEP_DEG,
        "bins": AZ_BINS,
        "minimum_deg": float(np.min(horizon)),
        "maximum_deg": float(np.max(horizon)),
        "mean_deg": float(np.mean(horizon)),
    }
    return horizon, diagnostics, fine, regional


def save_local_terrain_map(source: TerrainSource) -> None:
    radius_m = 6_000.0
    path = "/vsicurl/" + source.url
    with rasterio.Env(**GDAL_ENV):
        with rasterio.open(path) as src:
            _, _, map_scale = local_projection_basis(src, source.site_x, source.site_y)
            rm = radius_m * map_scale
            win = clip_window(
                from_bounds(source.site_x - rm, source.site_y - rm, source.site_x + rm, source.site_y + rm, transform=src.transform),
                src.width,
                src.height,
            )
            target = 1400
            scale = max(1, int(max(win.width, win.height) / target))
            out_h = max(1, int(win.height / scale))
            out_w = max(1, int(win.width / scale))
            arr = src.read(1, window=win, out_shape=(out_h, out_w), masked=True, resampling=rasterio.enums.Resampling.bilinear)
            z = raw_to_elevation(np.asarray(arr, dtype=np.float64))
            mask = np.ma.getmaskarray(arr)
            z = np.ma.array(z, mask=mask)
            fig, ax = plt.subplots(figsize=(10, 8))
            image = ax.imshow(z, origin="upper", extent=[-radius_m / 1000, radius_m / 1000, -radius_m / 1000, radius_m / 1000])
            ax.plot(0, 0, marker="+", markersize=14)
            ax.set_xlabel("Approximate east-west offset (km)")
            ax.set_ylabel("Approximate north-south offset (km)")
            ax.set_title(f"CE-7 modeled point: local public terrain\n{source.label}; native nominal pixel {source.resolution_m:g} m")
            fig.colorbar(image, ax=ax, label="Elevation relative to 1,737.4 km lunar sphere (m)")
            fig.tight_layout()
            fig.savefig(OUT / "ce7_local_terrain_map.png", dpi=220)
            plt.close(fig)


def load_kernels() -> list[Path]:
    paths = []
    for name, url in KERNEL_URLS.items():
        p = download(url, CACHE / name)
        paths.append(p)
        spice.furnsh(str(p))
    return paths


def spice_positions(ets: np.ndarray, frame: str = "MOON_ME") -> np.ndarray:
    chunks = []
    size = 20_000
    for start in range(0, ets.size, size):
        part = ets[start : start + size]
        try:
            pos, _ = spice.spkpos("SUN", part, frame, "LT+S", "MOON")
            chunks.append(np.asarray(pos, dtype=np.float64))
        except Exception:
            rows = []
            for et in part:
                pos, _ = spice.spkpos("SUN", float(et), frame, "LT+S", "MOON")
                rows.append(pos)
            chunks.append(np.asarray(rows, dtype=np.float64))
        log(f"Ephemeris: {min(start + size, ets.size):,}/{ets.size:,}")
    return np.vstack(chunks)


def visible_disk_fraction(delta_deg: np.ndarray, radius_deg: np.ndarray) -> np.ndarray:
    d = delta_deg / radius_deg
    result = np.empty_like(d, dtype=np.float64)
    result[d <= -1.0] = 0.0
    result[d >= 1.0] = 1.0
    middle = (d > -1.0) & (d < 1.0)
    x = d[middle]
    result[middle] = 0.5 + (np.arcsin(x) + x * np.sqrt(np.maximum(0.0, 1.0 - x * x))) / np.pi
    return result


def compute_illumination(horizon: np.ndarray) -> pd.DataFrame:
    kernel_paths = load_kernels()
    times = pd.date_range(START_UTC, END_UTC_EXCLUSIVE, freq=f"{STEP_SECONDS}s", inclusive="left", tz="UTC")
    et0 = float(spice.str2et(START_UTC))
    ets = et0 + np.arange(times.size, dtype=np.float64) * STEP_SECONDS
    positions = spice_positions(ets)
    distances_km = np.linalg.norm(positions, axis=1)
    sun = positions / distances_km[:, None]

    lat = math.radians(SITE_LAT_DEG)
    lon = math.radians(SITE_LON_E_DEG)
    up = np.array([math.cos(lat) * math.cos(lon), math.cos(lat) * math.sin(lon), math.sin(lat)])
    east = np.array([-math.sin(lon), math.cos(lon), 0.0])
    north = np.array([-math.sin(lat) * math.cos(lon), -math.sin(lat) * math.sin(lon), math.cos(lat)])

    u = sun @ up
    e = sun @ east
    n = sun @ north
    elevation = np.degrees(np.arcsin(np.clip(u, -1.0, 1.0)))
    azimuth = np.degrees(np.arctan2(e, n)) % 360.0

    az_centers = (np.arange(AZ_BINS, dtype=np.float64) + 0.5) * AZ_STEP_DEG
    xp = np.concatenate([az_centers - 360.0, az_centers, az_centers + 360.0])
    fp = np.concatenate([horizon, horizon, horizon])
    terrain_horizon = np.interp(azimuth, xp, fp)
    angular_radius = np.degrees(np.arcsin(np.clip(SUN_RADIUS_KM / distances_km, 0.0, 1.0)))
    clearance = elevation - terrain_horizon
    fraction = visible_disk_fraction(clearance, angular_radius)

    df = pd.DataFrame(
        {
            "utc": times.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "solar_azimuth_deg": azimuth,
            "solar_center_elevation_deg": elevation,
            "terrain_horizon_deg": terrain_horizon,
            "solar_angular_radius_deg": angular_radius,
            "center_clearance_deg": clearance,
            "visible_solar_disk_fraction": fraction,
        }
    )
    df.attrs["kernel_paths"] = [str(p) for p in kernel_paths]
    return df


def make_main_chart(df: pd.DataFrame) -> None:
    fraction = df["visible_solar_disk_fraction"].to_numpy()
    intervals_per_day = 24 * 3600 // STEP_SECONDS
    if fraction.size % intervals_per_day:
        raise RuntimeError("Time series does not contain whole UTC days")
    matrix = fraction.reshape((-1, intervals_per_day))
    day_index = pd.date_range("2026-01-01", "2030-12-31", freq="D", tz="UTC")
    if len(day_index) != matrix.shape[0]:
        raise RuntimeError("Date/matrix mismatch")

    fig, ax = plt.subplots(figsize=(16, 22))
    image = ax.imshow(matrix, aspect="auto", origin="upper", extent=[0, 24, matrix.shape[0], 0], vmin=0, vmax=1)
    ax.set_xlabel("UTC hour")
    ax.set_ylabel("UTC date")
    ax.set_xticks(np.arange(0, 25, 2))
    tick_dates = []
    tick_rows = []
    for year in range(2026, 2031):
        for month in (1, 7):
            date = pd.Timestamp(year=year, month=month, day=1, tz="UTC")
            if date <= day_index[-1]:
                tick_dates.append(date.strftime("%Y-%m-%d"))
                tick_rows.append((date - day_index[0]).days)
    ax.set_yticks(tick_rows)
    ax.set_yticklabels(tick_dates)
    ax.set_title(
        "CE-7 modeled landing point — terrain-aware solar visibility, 2026–2030\n"
        f"{abs(SITE_LAT_DEG):.1f}°S, {SITE_LON_E_DEG:.1f}°E · {OBSERVER_HEIGHT_M:g} m observer · {STEP_SECONDS // 60}-minute samples"
    )
    cbar = fig.colorbar(image, ax=ax, pad=0.015)
    cbar.set_label("Fraction of apparent solar disk above terrain horizon")
    fig.text(
        0.5,
        0.015,
        "Public LOLA terrain; lunar curvature and finite solar disk included. Nominal-point planning product—not a landing-ellipse or spacecraft-shadow analysis.",
        ha="center",
        fontsize=10,
    )
    fig.tight_layout(rect=[0, 0.025, 1, 1])
    fig.savefig(OUT / "ce7_illumination_chart_2026_2030.png", dpi=220)
    fig.savefig(OUT / "ce7_illumination_chart_2026_2030.pdf")
    plt.close(fig)


def make_horizon_chart(horizon: np.ndarray) -> None:
    az = (np.arange(AZ_BINS) + 0.5) * AZ_STEP_DEG
    fig, ax = plt.subplots(figsize=(14, 6))
    ax.plot(az, horizon)
    ax.set_xlim(0, 360)
    ax.set_xlabel("True lunar azimuth (degrees clockwise from north)")
    ax.set_ylabel("Terrain horizon elevation (degrees)")
    ax.set_title("Terrain horizon at the CE-7 modeled point")
    ax.grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(OUT / "ce7_terrain_horizon.png", dpi=220)
    plt.close(fig)


def write_horizon_csv(horizon: np.ndarray) -> None:
    with (OUT / "ce7_terrain_horizon.csv").open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(["azimuth_deg", "horizon_elevation_deg"])
        for i, value in enumerate(horizon):
            writer.writerow([(i + 0.5) * AZ_STEP_DEG, float(value)])


def write_time_series(df: pd.DataFrame) -> None:
    csv_path = OUT / "ce7_illumination_2026_2030_10min.csv.gz"
    with gzip.open(csv_path, "wt", newline="", encoding="utf-8", compresslevel=9) as fh:
        df.to_csv(fh, index=False)

    times = pd.to_datetime(df["utc"], utc=True)
    temp = pd.DataFrame({"date": times.dt.strftime("%Y-%m-%d"), "fraction": df["visible_solar_disk_fraction"]})
    daily = temp.groupby("date", as_index=False)["fraction"].sum()
    daily["effective_visible_sun_hours"] = daily["fraction"] * STEP_SECONDS / 3600.0
    daily.drop(columns=["fraction"]).to_csv(OUT / "ce7_daily_illumination_2026_2030.csv", index=False)

    state = np.where(df["visible_solar_disk_fraction"].to_numpy() <= 1e-9, "dark", np.where(df["visible_solar_disk_fraction"].to_numpy() >= 1 - 1e-9, "full", "partial"))
    changed = np.r_[True, state[1:] != state[:-1]]
    transitions = df.loc[changed, ["utc", "visible_solar_disk_fraction", "solar_azimuth_deg", "solar_center_elevation_deg", "terrain_horizon_deg"]].copy()
    transitions.insert(1, "state", state[changed])
    transitions.to_csv(OUT / "ce7_illumination_transitions_2026_2030.csv", index=False)


def yearly_summary(df: pd.DataFrame) -> pd.DataFrame:
    times = pd.to_datetime(df["utc"], utc=True)
    fraction = df["visible_solar_disk_fraction"].to_numpy()
    rows = []
    for year in range(2026, 2031):
        mask = times.dt.year.to_numpy() == year
        rows.append(
            {
                "year": year,
                "effective_visible_sun_hours": float(fraction[mask].sum() * STEP_SECONDS / 3600.0),
                "full_sun_hours": float(np.count_nonzero(fraction[mask] >= 1 - 1e-9) * STEP_SECONDS / 3600.0),
                "partial_sun_hours": float(np.count_nonzero((fraction[mask] > 1e-9) & (fraction[mask] < 1 - 1e-9)) * STEP_SECONDS / 3600.0),
                "dark_hours": float(np.count_nonzero(fraction[mask] <= 1e-9) * STEP_SECONDS / 3600.0),
            }
        )
    result = pd.DataFrame(rows)
    result.to_csv(OUT / "ce7_yearly_illumination_summary.csv", index=False)
    return result


def write_readme(diagnostics: dict, yearly: pd.DataFrame) -> None:
    sources = diagnostics.get("sources", [])
    source_lines = "\n".join(
        f"- **{s['label']}** — nominal pixel {s['resolution_m']:.3f} m; used from {s['min_radius_m']/1000:.1f} to {s['max_radius_m']/1000:.1f} km; {s['url']}"
        for s in sources
    )
    yearly_table = yearly.to_markdown(index=False, floatfmt=".2f")
    warning = diagnostics.get("regional_warning")
    warning_text = f"\n> **Regional terrain warning:** {warning}\n" if warning else ""
    text = f"""# CE-7 terrain-aware illumination package

This package was rebuilt after the original chat attachments failed to materialize. Every file in this ZIP is generated in the same run and listed with a SHA-256 digest in `manifest.json`.

## Modeled point

- Latitude: **{SITE_LAT_DEG:.4f}°** (planetocentric; south negative)
- Longitude: **{SITE_LON_E_DEG:.4f}° east**
- Observer height: **{OBSERVER_HEIGHT_M:.1f} m** above the DEM surface
- Time range: **2026-01-01 through 2030-12-31 UTC**
- Time step: **{STEP_SECONDS // 60} minutes**

This is a nominal-point planning calculation, not an assertion that the point is the final certified touchdown coordinate.

## Terrain

{source_lines}

The horizon is computed in {AZ_STEP_DEG:.1f}° azimuth bins. Each terrain pixel is evaluated in Moon-centered spherical geometry, including lunar curvature. Fine terrain controls the near field; a coarser regional product extends the horizon where available. The regional DEM is vertically aligned to the fine DEM at the modeled point before merging.
{warning_text}
## Solar geometry

The apparent Sun direction is computed with NAIF SPICE in the `MOON_ME` frame using:

- `de421.bsp`
- `moon_pa_de421_1900-2050.bpc`
- `moon_080317.tf`
- `pck00010.tpc`
- `naif0012.tls`

The Sun is treated as a finite disk. `visible_solar_disk_fraction` is the area fraction of that disk above the terrain horizon. A completely visible disk is 1; complete occultation is 0.

Future leap seconds not represented in `naif0012.tls` could shift post-2026 UTC labels by seconds, far below the 10-minute chart resolution.

## Yearly totals

{yearly_table}

## Files

- `ce7_illumination_chart_2026_2030.png` — primary high-resolution chart
- `ce7_illumination_chart_2026_2030.pdf` — print/vector container version
- `ce7_terrain_horizon.png` and `.csv` — terrain horizon profile
- `ce7_local_terrain_map.png` — local DEM context
- `ce7_illumination_2026_2030_10min.csv.gz` — complete time series
- `ce7_illumination_transitions_2026_2030.csv` — full/partial/dark transitions
- `ce7_daily_illumination_2026_2030.csv` — daily effective visible-sun hours
- `ce7_yearly_illumination_summary.csv` — yearly totals
- `terrain_diagnostics.json` — selected source metadata and processing diagnostics
- `manifest.json` — sizes and SHA-256 digests

## Limits

The calculation does **not** include:

- landing-ellipse uncertainty or spatial probability;
- rocks or boulders below the DEM resolution;
- lander, mast, solar-array, or payload self-shadowing;
- local slope-dependent panel incidence or power conversion;
- dust, scattered light, Earthshine, or thermal modeling;
- terrain changes or improved unpublished mission data.

Near grazing illumination, modest movement within the landing area may change the controlling ridge and transition time. Operational use should therefore extend this nominal-point result to an ensemble across the official landing ellipse.
"""
    (OUT / "CE7_illumination_README.md").write_text(text, encoding="utf-8")


def build_manifest() -> dict:
    entries = []
    for path in sorted(OUT.iterdir()):
        if not path.is_file() or path.name in {"manifest.json", "CE7_illumination_package.zip"}:
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        entries.append({"name": path.name, "size_bytes": path.stat().st_size, "sha256": digest})
    manifest = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "modeled_point": {"latitude_deg": SITE_LAT_DEG, "longitude_east_deg": SITE_LON_E_DEG},
        "files": entries,
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def build_zip() -> Path:
    destination = OUT / "CE7_illumination_package.zip"
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in sorted(OUT.iterdir()):
            if path.is_file() and path != destination:
                zf.write(path, arcname=path.name)
    with zipfile.ZipFile(destination, "r") as zf:
        bad = zf.testzip()
        if bad is not None:
            raise RuntimeError(f"ZIP integrity failure at {bad}")
    return destination


def verify_outputs() -> None:
    from PIL import Image

    required = [
        OUT / "ce7_illumination_chart_2026_2030.png",
        OUT / "CE7_illumination_package.zip",
        OUT / "CE7_illumination_README.md",
        OUT / "ce7_illumination_2026_2030_10min.csv.gz",
    ]
    for path in required:
        if not path.exists() or path.stat().st_size == 0:
            raise RuntimeError(f"Missing output: {path}")
    with Image.open(required[0]) as image:
        image.verify()
    with gzip.open(required[3], "rt", encoding="utf-8") as fh:
        header = fh.readline()
        first = fh.readline()
        if not header or not first:
            raise RuntimeError("Compressed CSV is empty")
    with zipfile.ZipFile(required[1], "r") as zf:
        if zf.testzip() is not None:
            raise RuntimeError("ZIP verification failed")


def main() -> None:
    log("Selecting and processing terrain")
    horizon, diagnostics, fine, regional = compute_horizon()
    write_horizon_csv(horizon)
    make_horizon_chart(horizon)
    save_local_terrain_map(fine)
    (OUT / "terrain_diagnostics.json").write_text(json.dumps(diagnostics, indent=2), encoding="utf-8")

    log("Computing solar ephemeris and terrain occultation")
    df = compute_illumination(horizon)
    make_main_chart(df)
    write_time_series(df)
    yearly = yearly_summary(df)
    write_readme(diagnostics, yearly)
    build_manifest()
    build_zip()
    verify_outputs()
    log("OUTPUT_VERIFIED")
    for path in sorted(OUT.iterdir()):
        if path.is_file():
            log(f"{path.name}\t{path.stat().st_size}")


if __name__ == "__main__":
    main()
