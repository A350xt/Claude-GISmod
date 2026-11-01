"""端到端演示：物流园区选址（纯矢量版）。

业务问题（对应 README 的 Phase-1 能力叙述）：
    在临江市寻找候选物流园区地块——
    距主干道不超过 2 km、避开洪水风险区、面积大于 1 km²，
    并回答每个行政区里各有多少候选面积。

运行：python scripts/demo_siting.py
产物：datasets/demo_out/artifacts/<artifact_id>/ 下的 result.gpkg 与 map.png
"""

from __future__ import annotations

import json
from pathlib import Path

import geopandas as gpd

from claude_gis import api

REPO = Path(__file__).resolve().parents[1]
DATA = REPO / "datasets" / "synthetic"
WORKDIR = REPO / "datasets" / "demo_out"


def main() -> None:
    ops = [
        {"id": "arterial", "op": "query.filter",
         "input": str(DATA / "roads.geojson"), "where": "`class` == '主干道'"},
        {"id": "reach", "op": "proximity.buffer",
         "input": "@arterial", "distance_m": 2000, "dissolve": True},
        {"id": "parcels_in_reach", "op": "overlay.intersection",
         "a": str(DATA / "parcels.geojson"), "b": "@reach"},
        {"id": "safe", "op": "overlay.difference",
         "a": "@parcels_in_reach", "b": str(DATA / "flood_zone.geojson")},
        {"id": "measured", "op": "query.measure",
         "input": "@safe", "measures": ["area_sqkm"]},
        {"id": "candidates", "op": "query.filter",
         "input": "@measured", "where": "area_sqkm > 1"},
        {"id": "by_district", "op": "zonal.summarize",
         "regions": str(DATA / "districts.geojson"), "data": "@candidates",
         "stats": ["count", "total_area_sqkm"]},
    ]
    analyze = api.analyze(WORKDIR, {
        "title": "临江市物流园区候选选址（矢量核心演示）",
        "operations": ops,
    })

    print("=" * 64)
    print("分析完成：", analyze["artifact_id"])
    print("统一度量坐标系：", analyze["crs"]["analysis"])
    print("-" * 64)
    print("分析过程：")
    for step in analyze["steps"]:
        print(f"  [{step['id']:>6}] {step['op']:<26} {step['count']:>3} 条")
    print("-" * 64)
    print("结论摘要：")
    for line in analyze["summary"]:
        if line.startswith("分区统计"):
            continue
        print(" •", line)

    candidates_ref = analyze["result"]["path"]
    # 最终步骤即"按行政区统计"表：直接用于排行
    ranked = _rank_districts(candidates_ref)
    print("-" * 64)
    print("各区候选面积排行（km²）：")
    for name, sqkm, cnt in ranked:
        print(f"  {name:<6} {sqkm:9.3f}   （{cnt} 个候选地块片段）")

    # 跨调用引用语法演示：把上一 artifact 的中间步骤 @candidates 当作输入
    followup = api.analyze(WORKDIR, {
        "title": "候选地块中的居住用地",
        "operations": [{
            "id": "res",
            "op": "query.filter",
            "input": f"{analyze['artifact_id']}#candidates",
            "where": "`landuse` == '居住'",
        }],
    })
    print("-" * 64)
    print("跨调用引用（ar_xxx#candidates → 居住过滤）：", followup["result"]["count"], "条")
    print("  ↳ follow-up artifact:", followup["artifact_id"])

    viz = api.visualize(WORKDIR, {
        "source": analyze["artifact_id"],
        "title": "各区候选物流园区面积",
        "style": {"mode": "choropleth", "field": "data_area_sqkm",
                  "classification": "quantile", "classes": 4},
    })
    print("-" * 64)
    print("结果数据集：", candidates_ref)
    print("专题地图  ：", viz["image_path"])

    meta_path = WORKDIR / "artifacts" / analyze["artifact_id"] / "meta.json"
    print("-" * 64)
    print("告警与留痕：")
    for w in analyze["warnings"]:
        print("  *", w)
    print("完整可追溯元数据：", meta_path)
    print("=" * 64)


def _rank_districts(result_path: str):
    gdf = gpd.read_file(result_path, layer="result", engine="pyogrio").sort_values(
        "data_area_sqkm", ascending=False)
    rows = []
    for _, row in gdf.iterrows():
        rows.append((row["name"], float(row["data_area_sqkm"]), int(row["data_count"])))
    return rows


if __name__ == "__main__":
    main()
