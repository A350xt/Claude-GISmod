"""面板支撑动作（preview/list）测试：Claude Code 侧边面板的数据通道。"""

from __future__ import annotations

import base64
from pathlib import Path

import pytest

from claude_gis.analysis.executor import execute_analyze
from claude_gis.api import list_inventory, preview
from claude_gis.protocol import E_INPUT_MISSING, GisError
from claude_gis.viz.preview import SVG_BUDGET, vector_svg

SYNTH = Path(__file__).resolve().parents[1] / "datasets" / "synthetic"

SITE_OPS = [
    {"id": "a", "op": "query.filter",
     "input": str(SYNTH / "roads.geojson"), "where": "`class` == '主干道'"},
    {"id": "b", "op": "proximity.buffer", "input": "@a", "distance_m": 2000, "dissolve": True},
    {"id": "c", "op": "overlay.intersection", "a": str(SYNTH / "parcels.geojson"), "b": "@b"},
]


def test_preview_dataset_svg(tmp_path):
    out = preview(tmp_path, {"source": str(SYNTH / "hospitals.geojson")})
    assert out["kind"] == "vector"
    assert out["meta"]["count"] == 12
    assert out["meta"]["geometry_types"] == {"Point": 12}
    assert out["svg"].startswith("<svg") and out["svg"].count('r="3.6"') == 12
    assert len(out["svg"]) <= SVG_BUDGET
    assert out["png_base64"] is None


def test_preview_png_for_terminal(tmp_path):
    out = preview(tmp_path, {"source": str(SYNTH / "parcels.geojson"), "png": True})
    raw = base64.b64decode(out["png_base64"])
    assert raw[1:4] == b"PNG"
    assert "<path" in out["svg"]


def test_preview_artifact_and_step(tmp_path):
    res = execute_analyze(tmp_path, {"title": "t", "operations": SITE_OPS})
    out = preview(tmp_path, {"source": res["artifact_id"]})
    assert out["meta"]["count"] == res["result"]["count"]
    step = preview(tmp_path, {"source": f"{res['artifact_id']}#a"})
    assert step["meta"]["count"] == 4  # 主干道 4 条
    assert step["meta"]["geometry_types"] == {"LineString": 4}


def test_preview_unknown_step_lists_available(tmp_path):
    res = execute_analyze(tmp_path, {"title": "t", "operations": SITE_OPS})
    with pytest.raises(GisError) as ei:
        preview(tmp_path, {"source": f"{res['artifact_id']}#nope"})
    assert ei.value.code == E_INPUT_MISSING


def test_preview_missing_source(tmp_path):
    with pytest.raises(GisError) as ei:
        preview(tmp_path, {"source": "D:/no/such/file.geojson"})
    assert ei.value.code == E_INPUT_MISSING


def test_preview_svg_stays_in_budget():
    """要素极多时逐级化简/抽样，SVG 永不超出面板 Svg 元素上限。"""
    import geopandas as gpd
    from shapely.geometry import Point

    pts = gpd.GeoDataFrame(geometry=[Point(i % 300, i // 300) for i in range(30_000)],
                           crs="EPSG:3857")
    svg = vector_svg(pts, "big")
    assert len(svg) <= SVG_BUDGET


def test_list_inventory(tmp_path):
    out = list_inventory(tmp_path, {"datasets_dir": str(SYNTH)})
    names = {d["name"] for d in out["datasets"]}
    assert {"parcels", "roads", "hospitals", "poi"} <= names
    assert out["artifacts"] == []

    res = execute_analyze(tmp_path, {"title": "清单测试", "operations": SITE_OPS})
    out = list_inventory(tmp_path, {"datasets_dir": str(SYNTH)})
    assert out["artifacts"][0]["artifact_id"] == res["artifact_id"]
    assert out["artifacts"][0]["count"] == res["result"]["count"]
    assert set(out["artifacts"][0]["intermediate_layers"]) == {"a", "b", "c"}


@pytest.mark.parametrize("crs,expect", [
    ("source", "EPSG:4326"),
    ("EPSG:3857", "EPSG:3857"),
    ("EPSG:4490", "EPSG:4490"),
    ("utm", "EPSG:32651"),
])
def test_preview_switch_crs(tmp_path, crs, expect):
    out = preview(tmp_path, {"source": str(SYNTH / "parcels.geojson"), "crs": crs})
    assert out["meta"]["crs"] == expect
    assert out["meta"]["source_crs"] == "EPSG:4326"
    assert expect.split(":")[1] in out["svg"]          # 坐标系徽标
    minx = out["meta"]["bounds"][0]
    assert (100 < minx < 130) if expect in ("EPSG:4326", "EPSG:4490") else minx > 1000


def test_preview_bad_crs(tmp_path):
    with pytest.raises(GisError) as ei:
        preview(tmp_path, {"source": str(SYNTH / "parcels.geojson"), "crs": "EPSG:abc"})
    assert ei.value.code == "E_BAD_REQUEST"


def test_visualize_in_chosen_crs(tmp_path):
    from claude_gis.api import show, visualize

    out = visualize(tmp_path, {"source": str(SYNTH / "districts.geojson"), "crs": "EPSG:3857",
                               "title": "墨卡托"})
    assert Path(out["image_path"]).exists()
    assert show(tmp_path, out["artifact_id"])["crs"]["analysis"] == "EPSG:3857"
