"""时间类操作：轨迹构建（点要素按时间排序连成线）。

典型场景：飓风定位点 → 路径线；车辆/船舶 GPS 点 → 行驶轨迹。
配合 datasource 的 datetime 自动识别，`time_field` 直接写字符串时间列即可。
"""

from __future__ import annotations

import geopandas as gpd
import pandas as pd
from shapely import LineString

from claude_gis.analysis.common import StepResult, need, opt
from claude_gis.protocol import E_BAD_REQUEST, E_EMPTY_RESULT, E_INPUT_MISSING, GisError, json_safe

TRACK_SUMMARY_LIMIT = 20


def handle_trajectory(resolver, params: dict, step_id: str) -> StepResult:
    time_field = str(need(params, "time_field", "trajectory.build"))
    group_by = opt(params, "group_by", None)
    gdf, _ = resolver.frame(need(params, "input", "trajectory.build"))

    if time_field not in gdf.columns:
        raise GisError(
            E_INPUT_MISSING,
            f"时间字段不存在：{time_field}",
            {"columns": [str(c) for c in gdf.columns]},
        )

    ts = gdf[time_field]
    if not pd.api.types.is_datetime64_any_dtype(ts):
        ts = pd.to_datetime(ts, errors="coerce")
    n_ok = int(ts.notna().sum())
    if n_ok < 2:
        raise GisError(
            E_BAD_REQUEST,
            f"time_field={time_field} 无法解析为时间（有效 {n_ok} / {len(gdf)} 条），"
            "请检查格式（支持 ISO 8601，如 2024-09-01T12:00:00）",
            {"time_field": time_field},
        )

    if group_by is not None:
        group_by = str(group_by)
        if group_by not in gdf.columns:
            raise GisError(
                E_INPUT_MISSING,
                f"分组字段不存在：{group_by}",
                {"columns": [str(c) for c in gdf.columns]},
            )

    work = gdf.loc[ts.notna()].copy()
    work["_t"] = ts[ts.notna()]
    work = work.sort_values("_t")

    label_col = group_by or "track"
    rows = []
    geoms = []
    skipped = 0
    for key, part in work.groupby(group_by if group_by else work["_t"].dt.date, dropna=False):
        if len(part) < 2:
            skipped += 1
            continue
        coords = [(p.x, p.y) for p in part.geometry]
        geoms.append(LineString(coords))
        start, end = part["_t"].iloc[0], part["_t"].iloc[-1]
        rows.append({
            label_col: json_safe(key),
            "point_count": int(len(part)),
            "start_time": json_safe(start),
            "end_time": json_safe(end),
            "duration_h": round((end - start).total_seconds() / 3600.0, 4),
        })

    if not geoms:
        raise GisError(
            E_EMPTY_RESULT,
            "没有任何分组能构成轨迹（每组至少需要 2 个带有效时间的点）",
            {"skipped_groups": skipped},
        )

    out = gpd.GeoDataFrame(rows, geometry=geoms, crs=gdf.crs)
    summaries = [
        f"轨迹构建：{len(out)} 条（按 {group_by or '全部点连线'}，时间字段 {time_field}，"
        f"有效点 {n_ok}/{len(gdf)}）"
    ]
    if skipped:
        summaries.append(f"{skipped} 个分组点数不足 2，已跳过")
    columns = list(rows[0].keys())
    table = {"columns": columns, "rows": [[r[c] for c in columns] for r in rows[:TRACK_SUMMARY_LIMIT]]}
    if len(rows) > TRACK_SUMMARY_LIMIT:
        table["note"] = f"表中仅前 {TRACK_SUMMARY_LIMIT} / {len(rows)} 条"
    return StepResult(out, summaries, table=table)


SPEC = {
    "trajectory.build": handle_trajectory,
}
