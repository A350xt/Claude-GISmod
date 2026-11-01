"""Step orchestration shared types."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class StepResult:
    gdf: object                      # geopandas.GeoDataFrame（矢量结果）
    summaries: list[str] = field(default_factory=list)
    """结构化明细表（如 measure 的分组汇总）：{columns: [...], rows: [[...], ...]}。

    直接进响应（steps[].table），让 Agent 不必再开 artifact 取数。"""
    table: dict[str, Any] | None = None
    """栅格结果（与 gdf 互斥）：float32 + NaN=nodata。"""
    raster: Any = None


def need(params: dict, key: str, op: str):
    if key not in params:
        raise KeyError(f"操作 {op} 缺少必填参数 {key}")
    return params[key]


def opt(params: dict, key: str, default):
    v = params.get(key, default)
    return default if v is None else v


def preview_table(gdf, limit: int = 10) -> dict:
    """结果预览表：前 limit 行、非几何列（最多 12 列），全部走 json_safe。"""
    from claude_gis.protocol import json_safe

    cols = [c for c in gdf.columns if c != gdf.geometry.name][:12]
    rows = gdf[cols].head(limit).to_dict("records") if cols else []
    return {
        "columns": [str(c) for c in cols],
        "rows": [json_safe(list(r.values())) for r in rows],
        "row_count_total": int(len(gdf)),
        "note": f"前 {min(limit, len(gdf))} / {len(gdf)} 行" if len(gdf) > limit else None,
    }
