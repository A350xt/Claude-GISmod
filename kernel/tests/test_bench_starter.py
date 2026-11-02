"""起始 benchmark：合成城市上的典型矢量问题。

期望值来源两级：
- construction truth（ground_truth.json，生成期统计，与 GIS 栈无关）；
- 结构性谓词（范围/不相交/投影）。
全量 150-200 题评测体系属于后续里程碑。
"""

from __future__ import annotations

import json
from pathlib import Path

import geopandas as gpd
import pytest

from conftest import REPO
from claude_gis.analysis.executor import execute_analyze

DATA = REPO / "datasets" / "synthetic"
BENCH = Path(__file__).parent / "bench" / "starter.jsonl"


def load_tasks():
    tasks = []
    for line in BENCH.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("//"):
            tasks.append(json.loads(line))
    return tasks


TASKS = load_tasks()


@pytest.mark.parametrize("task", TASKS, ids=[t["id"] for t in TASKS])
def test_starter_benchmark(task, tmp_path):
    ops = json.loads(json.dumps(task["operations"]).replace("{DATA}", str(DATA).replace("\\", "/")))
    result = execute_analyze(tmp_path, {"title": task["id"], "operations": ops})
    exp = task["expect"]

    count = int(result["result"]["count"])
    gt = json.load(open(DATA / "ground_truth.json", encoding="utf-8"))
    construct = gt["counts_from_construction"]
    aggregates = gt["aggregates"]

    if "count_equals" in exp:
        assert count == exp["count_equals"]
    if "count_equals_field" in exp:
        key, sub = exp["count_equals_field"]
        lookup = {**construct, **aggregates}
        value = lookup[key]
        if sub is not None:
            value = value[sub]
        assert count == value, f"{task['id']}: got {count}, want {key}[{sub}]={value}"
    if "count_equals_field_flat" in exp:
        assert count == construct[exp["count_equals_field_flat"][0]]
    if "count_between" in exp:
        lo, hi = exp["count_between"]
        assert lo <= count <= hi
    if "min_count" in exp:
        assert count >= exp["min_count"]
    if "max_count" in exp:
        assert count <= exp["max_count"]

    final_gdf = gpd.read_file(result["result"]["path"],
                              layer="result", engine="pyogrio") \
        if result["result"]["format"] == "gpkg" else \
        gpd.read_file(result["result"]["path"])

    if "has_columns" in exp:
        missing = [c for c in exp["has_columns"] if c not in final_gdf.columns]
        assert not missing, f"缺列:{missing}"
    if "col_sum_equals" in exp:
        col, want = exp["col_sum_equals"]
        total = float(final_gdf[col].sum())
        assert total == pytest.approx(want), f"{col} 合计 {total} != {want}"
    if "col_sum_equals_agg" in exp:
        col, agg_key = exp["col_sum_equals_agg"]
        total = float(final_gdf[col].sum())
        assert total == pytest.approx(float(aggregates[agg_key])), f"{col} != {agg_key}"
    if "col_sum_within_pct_of_agg" in exp:
        col, scope, key, min_pct = exp["col_sum_within_pct_of_agg"]
        source = construct if scope == "counts" else aggregates
        full = float(source[key])
        got = float(final_gdf[col].sum())
        assert got <= full * 1.0001, f"{col} 覆盖合计 {got} 超过全量真值 {full}"
        assert got >= full * min_pct / 100.0, (
            f"{col} 覆盖率不足：{got:.1f}/{full:.1f} < {min_pct}%"
            f"（合成城市区界存在收缩缝隙属预期，见 ground_truth 说明）"
        )
    if "disjoint_ref" in exp:
        ref_path = exp["disjoint_ref"].replace("{DATA}", str(DATA))
        ref = gpd.read_file(ref_path, engine="pyogrio").union_all()
        overlaps = final_gdf.geometry.intersects(ref)
        n_overlap = int(sum(bool(v) for v in overlaps))
        assert n_overlap == 0, f"{n_overlap} 个结果要素仍与参照层相交"
    if "summary_contains_any_km" in exp:
        lower, upper = exp["summary_contains_any_km"]
        joined = "".join(result["summary"])
        km_value = _parse_first_km(joined)
        assert km_value is not None and lower < km_value < upper
    if exp.get("projected_analysis"):
        from pyproj import CRS

        assert CRS.from_user_input(result["crs"]["analysis"]).is_projected


def _parse_first_km(text: str) -> float | None:
    idx = text.find("km")
    if idx < 0:
        return None
    head = text[:idx]
    num = ""
    for ch in reversed(head):
        if ch.isdigit() or ch in ".,":
            num = ch + num
        elif num:
            break
    try:
        return float(num.replace(",", ""))
    except ValueError:
        return None
