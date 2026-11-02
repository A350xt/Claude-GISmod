"""Query & Measure 原语测试，含 CRS 陷阱回归。"""

from __future__ import annotations

import geopandas as gpd
import pytest
from shapely.geometry import box

from claude_gis.analysis.resolver import DatasetResolver
from claude_gis.analysis.query import handle_filter, handle_measure, handle_select
from claude_gis.protocol import E_BAD_REQUEST, GisError
from claude_gis.runtime.artifact import ArtifactStore

from conftest import UTM
from helpers import write


@pytest.fixture()
def resolver(workdir):
    return DatasetResolver(ArtifactStore(workdir))


def test_filter_counts(resolver, synthetic_dir):
    res = handle_filter(resolver, {
        "input": str(synthetic_dir / "parcels.geojson"),
        "where": "`landuse` == '工业'",
    }, "s1")
    assert len(res.gdf) == 7          # ground_truth: 工业=7
    assert "7 条命中" in res.summaries[0]


def test_filter_keyword_column_backtick(resolver, synthetic_dir):
    """`class` 是 Python 关键字：必须用反引号语法（文档化约定）。"""
    res = handle_filter(resolver, {
        "input": str(synthetic_dir / "roads.geojson"),
        "where": "`class` == '高速'",
    }, "s1")
    assert len(res.gdf) == 2
    assert set(res.gdf["road_id"]) == {"R_H1", "R_H2"}


def test_filter_numeric_and_logic(resolver, synthetic_dir):
    res = handle_filter(resolver, {
        "input": str(synthetic_dir / "parcels.geojson"),
        "where": "pop_density > 20000",
    }, "s")
    assert len(res.gdf) >= 1
    assert res.gdf["pop_density"].min() > 20000


def test_filter_dangerous_tokens_rejected(resolver, workdir, sq_1km):
    sq_path = write(sq_1km, "sq.geojson", workdir)
    with pytest.raises(GisError) as ei:
        handle_filter(resolver, {"input": sq_path,
                                 "where": "__import__('os').system('x')"}, "s")
    assert ei.value.code == E_BAD_REQUEST


def test_measure_projected_exact_km2(resolver, workdir, sq_1km):
    p = write(sq_1km, "sq_proj.geojson", workdir)
    res = handle_measure(resolver, {"input": p, "measures": ["area_sqkm"]}, "s")
    got = float(res.gdf["area_sqkm"].iloc[0])
    assert got == pytest.approx(1.0, abs=1e-6)
    assert "总面积 1.000 km²" in res.summaries[0]


def test_measure_degrees_square_no_degree_trap(resolver, workdir):
    """EPSG:4326 下 0.01°×0.01° 若按度数直算面积会错若干数量级；统一投影后 ≈1 km²。"""
    square = gpd.GeoDataFrame({"name": ["d1"]},
                              geometry=[box(121.47, 31.23, 121.48, 31.24)],
                              crs="EPSG:4326")
    p = write(square, "sq_deg.geojson", workdir)
    res = handle_measure(resolver, {"input": p, "measures": ["area_sqkm"]}, "s")
    got = float(res.gdf["area_sqkm"].iloc[0])
    assert 0.8 < got < 1.3            # 真实地面约 1.05 km²


def test_select_spatial_intersects(resolver, workdir, two_regions_points_lines):
    regions, pts, _line = two_regions_points_lines
    left_path = write(regions[regions["rname"] == "R左"], "left.geojson", workdir)
    pts_path = write(pts, "pts.geojson", workdir)
    res = handle_select(resolver, {"input": pts_path, "predicate": "intersects",
                                   "ref": left_path}, "s")
    assert len(res.gdf) == 4           # 左半区 4 个点
