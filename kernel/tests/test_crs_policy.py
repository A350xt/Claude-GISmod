"""CRS 策略回归（实测反馈第 5 条：纯属性过滤不应悄悄重投影）。"""

from __future__ import annotations

from pathlib import Path

import geopandas as gpd
from shapely.geometry import Point, Polygon

from claude_gis import api


def _pts(tmp_path: Path) -> Path:
    gdf = gpd.GeoDataFrame(
        {"v": [1, 2, 3]},
        geometry=[Point(121.4, 31.2), Point(121.5, 31.25), Point(121.6, 31.3)],
        crs="EPSG:4326",
    )
    p = tmp_path / "pts.geojson"
    gdf.to_file(p, driver="GeoJSON")
    return p


def _polys(tmp_path: Path) -> Path:
    gdf = gpd.GeoDataFrame(
        {"z": ["a", "b"]},
        geometry=[
            Polygon([(121.3, 31.1), (121.5, 31.1), (121.5, 31.3), (121.3, 31.3)]),
            Polygon([(121.5, 31.1), (121.7, 31.1), (121.7, 31.3), (121.5, 31.3)]),
        ],
        crs="EPSG:4326",
    )
    p = tmp_path / "zones.geojson"
    gdf.to_file(p, driver="GeoJSON")
    return p


def test_pure_filter_keeps_source_crs(tmp_path: Path):
    pts = _pts(tmp_path)
    result = api.analyze(tmp_path, {
        "title": "纯过滤",
        "operations": [{"id": "f", "op": "query.filter",
                        "input": str(pts), "where": "v >= 2"}],
    })
    assert result["crs"]["analysis"] == "EPSG:4326"
    assert not any("重投影" in w for w in result["warnings"])


def test_spatial_select_keeps_source_crs(tmp_path: Path):
    result = api.analyze(tmp_path, {
        "title": "拓扑选择",
        "operations": [{"id": "s", "op": "query.select",
                        "input": str(_pts(tmp_path)), "predicate": "within",
                        "ref": str(_polys(tmp_path))}],
    })
    assert result["crs"]["analysis"] == "EPSG:4326"


def test_metric_op_still_reprojects(tmp_path: Path):
    result = api.analyze(tmp_path, {
        "title": "度量",
        "operations": [{"id": "m", "op": "query.measure",
                        "input": str(_polys(tmp_path)), "measures": ["area_sqkm"]}],
    })
    assert result["crs"]["analysis"].startswith("EPSG:326")  # UTM
    assert any("统一坐标系" in w for w in result["warnings"])


def test_explicit_crs_override(tmp_path: Path):
    result = api.analyze(tmp_path, {
        "title": "指定坐标系",
        "crs": "EPSG:32651",
        "operations": [{"id": "m", "op": "query.measure",
                        "input": str(_polys(tmp_path)), "measures": ["area_sqkm"]}],
    })
    assert result["crs"]["analysis"] == "EPSG:32651"


def test_invalid_crs_rejected(tmp_path: Path):
    import pytest

    from claude_gis.protocol import GisError

    with pytest.raises(GisError) as ei:
        api.analyze(tmp_path, {
            "title": "坏 CRS",
            "crs": "EPSG:99999",
            "operations": [{"id": "m", "op": "query.measure",
                            "input": str(_polys(tmp_path)), "measures": ["area_sqkm"]}],
        })
    assert ei.value.code == "E_BAD_REQUEST"
    assert "EPSG:32622" in ei.value.message  # 错误信息自带示例，可行动
