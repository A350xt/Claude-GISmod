"""Proximity 原语测试。"""

from __future__ import annotations

import pytest
from shapely.geometry import Point, box

from claude_gis.analysis.proximity import (handle_buffer, handle_nearest,
                                        handle_within_distance)
from claude_gis.analysis.resolver import DatasetResolver
from claude_gis.protocol import E_EMPTY_RESULT, GisError
from claude_gis.runtime.artifact import ArtifactStore

from helpers import write


@pytest.fixture()
def resolver(workdir):
    return DatasetResolver(ArtifactStore(workdir))


def test_buffer_enlarges_square_exactly(resolver, workdir, sq_1km):
    p = write(sq_1km, "sq.geojson", workdir)
    res = handle_buffer(resolver, {"input": p, "distance_m": 100, "dissolve": False}, "b1")
    area_m2 = float(res.gdf.geometry.area.iloc[0])
    # 解析解：外扩方块 1200² 内，四个直角被四分之一圆替换：净变化 = −(4−π)r²
    expected = 1200**2 - (4 - __import__("math").pi) * 100**2
    assert area_m2 == pytest.approx(expected, rel=5e-4)
    assert res.gdf["buffer_distance_m"].iloc[0] == 100


def test_buffer_dissolve_single_feature(resolver, workdir, two_regions_points_lines):
    _r, pts, _line = two_regions_points_lines
    p = write(pts.head(2), "pts.geojson", workdir)   # 相距约 1131 m 的两点
    res = handle_buffer(resolver, {"input": p, "distance_m": 300, "dissolve": True}, "b")
    assert len(res.gdf) == 1


def test_within_distance_picks_correct_side(resolver, workdir):
    ref = box(0, 0, 200, 200)
    ref_gdf = _gdf([ref])
    near = Point(250, 100)     # 距 ref 50 m
    far = Point(1500, 500)     # 距 ref 1300 m
    pts = _gdf([near, far], {"pid": ["近", "远"]})
    ref_path = write(ref_gdf, "ref.geojson", workdir)
    pts_path = write(pts, "pts.geojson", workdir)
    res = handle_within_distance(resolver, {"input": pts_path, "ref": ref_path,
                                            "distance_m": 100}, "w")
    assert list(res.gdf["pid"]) == ["近"]


def test_nearest_orders_and_joins_attrs(resolver, workdir):
    hospitals = _gdf([Point(0, 0), Point(3000, 0)],
                     {"hname": ["东院", "西院"], "beds": [900, 400]})
    village = _gdf([Point(2600, 100)], {"vname": ["赵家湾"]})
    h_path = write(hospitals, "h.geojson", workdir)
    v_path = write(village, "v.geojson", workdir)
    res = handle_nearest(resolver, {"input": v_path, "ref": h_path, "k": 2,
                                    "join_attrs": ["hname", "beds"]}, "n")
    assert len(res.gdf) == 2                          # k=2 → 两行
    first = res.gdf.sort_values("nearest_rank").iloc[0]
    second = res.gdf.sort_values("nearest_rank").iloc[1]
    assert first["nearest_hname"] == "西院"           # 最近的是 (3000,0)
    assert float(first["nearest_distance_m"]) < float(second["nearest_distance_m"])
    assert int(second["nearest_beds"]) == 900         # 第二近是原点医院


def test_nearest_empty_ref_raises(resolver, workdir, sq_1km):
    empty = sq_1km.iloc[0:0]
    v_path = write(_gdf([Point(0, 0)], {"pid": ["p"]}), "p.geojson", workdir)
    e_path = write(empty, "empty.geojson", workdir)
    with pytest.raises(GisError) as ei:
        handle_nearest(resolver, {"input": v_path, "ref": e_path}, "n")
    assert ei.value.code == E_EMPTY_RESULT


def _gdf(geoms, attrs: dict | None = None):
    import geopandas as gpd

    data = dict(attrs or {})
    return gpd.GeoDataFrame(data, geometry=list(geoms), crs="EPSG:32651")
