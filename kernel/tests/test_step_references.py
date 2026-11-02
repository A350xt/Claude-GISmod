"""跨调用中间步骤引用（ar_xxx#step）回归测试。

防止重现"artifact 只存最终结果，二次分析误用最终表当数据层"的静默错误。
"""

from __future__ import annotations

from pathlib import Path

import geopandas as gpd
import pytest

from claude_gis.analysis.executor import execute_analyze
from claude_gis.protocol import E_INPUT_MISSING, GisError

SYNTH = Path(__file__).resolve().parents[1] / "datasets" / "synthetic"

SITE_OPS = [
    {"id": "a", "op": "query.filter",
     "input": str(SYNTH / "roads.geojson"), "where": "`class` == '主干道'"},
    {"id": "b", "op": "proximity.buffer", "input": "@a", "distance_m": 2000, "dissolve": True},
    {"id": "c", "op": "overlay.intersection", "a": str(SYNTH / "parcels.geojson"), "b": "@b"},
    {"id": "d", "op": "overlay.difference", "a": "@c", "b": str(SYNTH / "flood_zone.geojson")},
]


def test_intermediate_steps_persisted_in_gpkg(tmp_path):
    result = execute_analyze(tmp_path, {"title": "t", "operations": SITE_OPS})
    gpkg = result["result"]["path"]
    layers = {str(n) for n in gpd.list_layers(gpkg)["name"]}
    assert "result" in layers
    assert "step_c" in layers and "step_d" in layers
    meta = result
    assert set(meta["intermediate_layers"]) >= {"a", "b", "c", "d"}


def test_second_call_reads_intermediate_layer(tmp_path):
    first = execute_analyze(tmp_path, {"title": "t1", "operations": SITE_OPS})
    token = f"{first['artifact_id']}#d"

    second = execute_analyze(tmp_path, {
        "title": "t2",
        "operations": [{"id": "res", "op": "query.filter",
                        "input": token, "where": "`landuse` == '居住'"}],
    })
    # 过滤后的数量不得超过中间步的整体要素数，且必须能从持久化图层读到
    inner_count = first["steps"][3]["count"]
    assert 0 < second["result"]["count"] <= inner_count
    assert second["inputs"][0]["step"] == "d"


def test_unknown_step_reference_fails_loudly(tmp_path):
    first = execute_analyze(tmp_path, {"title": "t", "operations": SITE_OPS})
    bad_token = f"{first['artifact_id']}#不存在"
    with pytest.raises(GisError) as ei:
        execute_analyze(tmp_path, {
            "title": "bad",
            "operations": [{"id": "x", "op": "query.measure",
                            "input": bad_token, "measures": ["count"]}],
        })
    assert ei.value.code == E_INPUT_MISSING


def test_final_table_ranking_matches_independent_recompute(tmp_path):
    """防静默错误回归：zonal 表的 data_count 总和须等于独立 sjoin 复算。"""
    site_ops = SITE_OPS + [
        {"id": "m", "op": "query.measure", "input": "@d", "measures": ["area_sqkm"]},
        {"id": "big", "op": "query.filter", "input": "@m", "where": "area_sqkm > 1"},
        {"id": "z", "op": "zonal.summarize",
         "regions": str(SYNTH / "districts.geojson"), "data": "@big",
         "stats": ["count", "total_area_sqkm"]},
    ]
    res = execute_analyze(tmp_path, {"title": "rank", "operations": site_ops})
    table = gpd.read_file(res["result"]["path"], layer="result", engine="pyogrio")

    cands = gpd.read_file(res["result"]["path"], layer="step_big", engine="pyogrio")
    regs = gpd.read_file(str(SYNTH / "districts.geojson"), engine="pyogrio").to_crs(
        cands.crs).reset_index(drop=True)
    joined = gpd.sjoin(cands.reset_index(drop=True),
                       regs.assign(_ridx=range(len(regs))),
                       predicate="intersects", how="left")
    independent = joined.groupby("_ridx")["parcel_id"].nunique()
    for idx, row in table.iterrows():
        expect = int(independent.get(idx, 0))
        assert int(row["data_count"]) == expect
