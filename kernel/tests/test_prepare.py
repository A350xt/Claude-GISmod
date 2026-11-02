"""数据准备管线（CRS 假定 / 几何修复）测试。"""

from __future__ import annotations

import geopandas as gpd
import pytest
from shapely.geometry import Point, Polygon, box

from claude_gis.protocol import E_CRS_AMBIGUOUS, GisError
from claude_gis.runtime.prepare import (assume_missing_crs, estimate_utm,
                                     repair_geometries)

from conftest import bowtie_gdf


def test_missing_crs_assumed_wgs84_with_warning():
    gdf = gpd.GeoDataFrame({"n": [1]}, geometry=[Point(121.5, 31.2)], crs=None)
    warnings: list[str] = []
    out = assume_missing_crs(gdf, "test.geojson", warnings)
    assert str(out.crs) == "EPSG:4326"
    assert any("EPSG:4326" in w for w in warnings)


def test_missing_crs_out_of_range_rejected():
    # UTM 米制坐标但声称无 CRS：绝不允许瞎猜
    gdf = gpd.GeoDataFrame({"n": [1]}, geometry=[Point(325000, 3450000)], crs=None)
    with pytest.raises(GisError) as ei:
        assume_missing_crs(gdf, "mystery.shp", [])
    assert ei.value.code == E_CRS_AMBIGUOUS


def test_bowtie_repaired_and_reported():
    gdf = bowtie_gdf()
    assert not gdf.geometry.iloc[0].is_valid
    warnings: list[str] = []
    out = repair_geometries(gdf, warnings)
    assert bool(out.geometry.is_valid.all())
    assert len(out) == 1                      # 修复而非丢要素
    assert any("修复" in w for w in warnings)
    area = float(out.geometry.area.sum())
    assert 0 < area < 5                       # 蝴蝶结拆成两个三角（度单位下面积约 4°²以下）


def test_null_geometry_dropped_with_warning():
    gdf = gpd.GeoDataFrame(
        {"n": [1, 2]},
        geometry=[Point(0, 0), None],
        crs="EPSG:4326",
    )
    warnings: list[str] = []
    out = repair_geometries(gdf, warnings)
    assert len(out) == 1
    assert any("无几何" in w or "空" in w for w in warnings)


def test_estimate_utm_near_shanghai():
    gdf = gpd.GeoDataFrame({"n": [1]}, geometry=[box(121.47, 31.23, 121.48, 31.24)],
                           crs="EPSG:4326")
    utm = estimate_utm(gdf)
    assert utm.to_epsg() == 32651             # 上海位于 51N 带


def test_make_valid_collection_simplified():
    # make_valid(蝴蝶结) 会产生 MultiPolygon；representative 化后仍须是合法几何
    from shapely import wkt

    bowtie = wkt.loads("POLYGON((0 0, 2 2, 2 0, 0 2, 0 0))")
    assert not bowtie.is_valid
    gdf = gpd.GeoDataFrame({"n": [1]}, geometry=[bowtie], crs="EPSG:4326")
    out = repair_geometries(gdf, [])
    assert bool(out.geometry.is_valid.all())
    assert len(out) == 1
