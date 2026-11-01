"""Overlay 原语测试：精确面积对拍。"""

from __future__ import annotations

import pytest

from claude_gis.analysis.overlay import (handle_clip, handle_difference,
                                      handle_intersection, handle_union)
from claude_gis.analysis.resolver import DatasetResolver
from claude_gis.runtime.artifact import ArtifactStore

from helpers import write

EXPECTED = {
    "inter_sqkm": 0.75,   # A=2×1km, B=1×1km 抬高 250m → 交 1000×750 m
    "diff_sqkm": 1.25,    # A − B
    "union_sqkm": 2.25,   # A ∪ B（B 上半 250m 在 A 外）
}


@pytest.fixture()
def paths(workdir, overlap_pair):
    a, b = overlap_pair
    return write(a, "a.geojson", workdir), write(b, "b.geojson", workdir)


@pytest.fixture()
def resolver(workdir):
    return DatasetResolver(ArtifactStore(workdir))


def test_intersection_exact(paths, resolver):
    ap, bp = paths
    res = handle_intersection(resolver, {"a": ap, "b": bp}, "i1")
    got = float(res.gdf.geometry.area.sum()) / 1e6
    assert got == pytest.approx(EXPECTED["inter_sqkm"], abs=1e-9)
    # 属性合并：A.tag + B.tag 均保留
    row = res.gdf.iloc[0]
    assert row["tag_a"] == "A" and row["tag_b"] == "B"


def test_difference_exact(paths, resolver):
    ap, bp = paths
    res = handle_difference(resolver, {"a": ap, "b": bp}, "d1")
    got = float(res.gdf.geometry.area.sum()) / 1e6
    assert got == pytest.approx(EXPECTED["diff_sqkm"], abs=1e-9)
    assert set(res.gdf.columns) >= {"tag"}          # 只保留 A 的属性


def test_union_exact(paths, resolver):
    ap, bp = paths
    res = handle_union(resolver, {"a": ap, "b": bp}, "u1")
    got = float(res.gdf.geometry.area.sum()) / 1e6
    assert got == pytest.approx(EXPECTED["union_sqkm"], abs=1e-9)


def test_clip_keeps_only_a_attrs(paths, resolver):
    ap, bp = paths
    res = handle_clip(resolver, {"a": ap, "b": bp}, "c1")
    assert float(res.gdf.geometry.area.sum()) / 1e6 == pytest.approx(
        EXPECTED["inter_sqkm"], abs=1e-9)
    assert "tag" in res.gdf.columns and "tag_b" not in res.gdf.columns
