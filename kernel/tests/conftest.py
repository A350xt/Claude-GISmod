"""Shared fixtures: exact-geometry fixtures in projected CRS + synthetic city."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import geopandas as gpd
import pandas as pd
import pytest
from shapely.geometry import LineString, Point, Polygon, box

REPO = Path(__file__).resolve().parents[1]
SYNTH = REPO / "datasets" / "synthetic"
UTM = "EPSG:32651"  # 上海附近 UTM 50N


@pytest.fixture(scope="session")
def synthetic_dir() -> Path:
    if not (SYNTH / "ground_truth.json").exists():
        subprocess.run([sys.executable, str(REPO / "scripts" / "make_synthetic_data.py")],
                       check=True, cwd=REPO)
    return SYNTH


@pytest.fixture(scope="session")
def ground_truth(synthetic_dir) -> dict:
    with open(synthetic_dir / "ground_truth.json", encoding="utf-8") as fh:
        return json.load(fh)


# --------------------------------------------------------------- 精确几何

@pytest.fixture()
def sq_1km() -> gpd.GeoDataFrame:
    """位于原点东侧的 1×1 km 正方形（投影坐标系）。"""
    return gpd.GeoDataFrame({"name": ["sq1"]}, geometry=[box(0, 0, 1000, 1000)], crs=UTM)


@pytest.fixture()
def overlap_pair():
    """A=2×1km 在 x∈[0,2000]；B=1×1km 在 x∈[500,1500]。精确交/差/并面积已知。"""
    a = gpd.GeoDataFrame({"tag": ["A"]}, geometry=[box(0, 0, 2000, 1000)], crs=UTM)
    b = gpd.GeoDataFrame({"tag": ["B"]}, geometry=[box(500, 250, 1500, 1250)], crs=UTM)
    return a, b


@pytest.fixture()
def two_regions_points_lines():
    regions = gpd.GeoDataFrame(
        {"rname": ["R左", "R右"]},
        geometry=[box(0, 0, 1000, 1000), box(1000, 0, 2000, 1000)],
        crs=UTM,
    )
    pts = gpd.GeoDataFrame(
        {"pid": range(8), "pop": [10, 20, 30, 40, 50, 60, 70, 80]},
        geometry=[Point(x + 5, y + 5) for x, y in
                  [(100, 100), (900, 900), (500, 500), (100, 800),
                   (1100, 100), (1900, 900), (1500, 500), (1950, 100)]],
        crs=UTM,
    )
    # 横穿两区的水平线：左区恰 1000 m，右区恰 1000 m
    line = gpd.GeoDataFrame(
        {"lid": ["L1"], "weight": [2.0]},
        geometry=[LineString([(0, 500), (2000, 500)])],
        crs=UTM,
    )
    return regions, pts, line


def bowtie_gdf() -> gpd.GeoDataFrame:
    """自相交蝴蝶结多边形（无效几何经典样例）。"""
    bad = Polygon([(0, 0), (2, 2), (2, 0), (0, 2)])
    assert not bad.is_valid
    return gpd.GeoDataFrame({"bid": ["bowtie"]}, geometry=[bad], crs="EPSG:4326")


@pytest.fixture()
def workdir(tmp_path) -> Path:
    d = tmp_path / "work"
    d.mkdir()
    return d


def write_geojson(gdf: gpd.GeoDataFrame, path: Path) -> Path:
    gdf.to_file(path, driver="GeoJSON", index=False)
    return path


_pd = pd  # noqa: F841  (保留导入以便未来 fixture 使用)
