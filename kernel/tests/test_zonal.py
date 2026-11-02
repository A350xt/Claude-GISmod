"""Zonal 统计测试。"""

from __future__ import annotations

import pytest

from claude_gis.analysis.resolver import DatasetResolver
from claude_gis.analysis.zonal import handle_zonal
from claude_gis.protocol import E_BAD_REQUEST, E_INPUT_MISSING, GisError
from claude_gis.runtime.artifact import ArtifactStore

from helpers import write


@pytest.fixture()
def resolver(workdir):
    return DatasetResolver(ArtifactStore(workdir))


def test_points_count_and_field_sum(resolver, workdir, two_regions_points_lines):
    regions, pts, _line = two_regions_points_lines
    rp = write(regions, "regions.geojson", workdir)
    pp = write(pts, "pts.geojson", workdir)
    res = handle_zonal(resolver, {"regions": rp, "data": pp,
                                  "stats": ["count", "sum"], "field": "pop"}, "z")
    out = res.gdf.set_index("rname")
    assert int(out.loc["R左", "data_count"]) == 4
    assert int(out.loc["R右", "data_count"]) == 4
    assert float(out.loc["R左", "data_pop_sum"]) == pytest.approx(10 + 20 + 30 + 40)
    total = float(out["data_pop_sum"].sum())
    assert total == pytest.approx(360.0)              # 全部点都被覆盖


def test_line_length_split_and_share(resolver, workdir, two_regions_points_lines):
    regions, _pts, line = two_regions_points_lines
    rp = write(regions, "regions.geojson", workdir)
    lp = write(line, "line.geojson", workdir)
    res = handle_zonal(resolver, {"regions": rp, "data": lp,
                                  "stats": ["total_length_km", "share_pct"]}, "z")
    out = res.gdf.set_index("rname")
    left_km = float(out.loc["R左", "data_length_km"])
    right_km = float(out.loc["R右", "data_length_km"])
    assert left_km == pytest.approx(1.0, abs=2e-3)
    assert right_km == pytest.approx(1.0, abs=2e-3)
    total_share = float(out["data_length_share_pct"].sum())
    assert total_share == pytest.approx(100.0, abs=1e-6)


def test_polygon_apportioned_sum_half_overlap(resolver, workdir):
    """多边形数据 50% 落入区域：apportioned sum 必须精确等于一半（防整块计数错误）。"""
    from shapely.geometry import box as _box

    region = _gdf([_box(0, 0, 1000, 1000)], {"rname": ["R"]})
    parcel_full = _box(500, 0, 1500, 1000)            # 一半在区内
    parcels = _gdf([parcel_full], {"value": [200.0]})
    rp = write(region, "region.geojson", workdir)
    dp = write(parcels, "parcels.geojson", workdir)
    res = handle_zonal(resolver, {"regions": rp, "data": dp,
                                  "stats": ["count", "total_area_sqkm", "share_pct",
                                            "sum"], "field": "value"}, "z")
    row = res.gdf.iloc[0]
    assert float(row["data_area_sqkm"]) == pytest.approx(0.5, abs=1e-9)
    assert float(row["data_area_share_pct"]) == pytest.approx(50.0)
    assert float(row["data_value_sum_apportioned"]) == pytest.approx(100.0, rel=1e-9)


def test_sum_without_field_rejected(resolver, workdir, two_regions_points_lines):
    regions, pts, _l = two_regions_points_lines
    rp = write(regions, "regions.geojson", workdir)
    pp = write(pts, "pts.geojson", workdir)
    with pytest.raises(GisError) as ei:
        handle_zonal(resolver, {"regions": rp, "data": pp, "stats": ["sum"]}, "z")
    assert ei.value.code == E_BAD_REQUEST


def test_missing_field_column_rejected(resolver, synthetic_dir):
    with pytest.raises(GisError) as ei:
        handle_zonal(resolver, {
            "regions": str(synthetic_dir / "districts.geojson"),
            "data": str(synthetic_dir / "parcels.geojson"),
            "stats": ["sum"], "field": "不存在的列",
        }, "z")
    assert ei.value.code == E_INPUT_MISSING


def _gdf(geoms, attrs=None):
    import geopandas as gpd

    return gpd.GeoDataFrame(dict(attrs or {}), geometry=list(geoms), crs="EPSG:32651")
