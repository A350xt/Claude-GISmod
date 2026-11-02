"""Proximity handlers."""

from __future__ import annotations

from claude_gis.analysis.common import StepResult, need, opt
from claude_gis.providers.geostack import PROVIDER


def handle_buffer(resolver, params: dict, step_id: str) -> StepResult:
    distance = float(need(params, "distance_m", "proximity.buffer"))
    if distance <= 0:
        raise ValueError("distance_m 必须为正数（单位：米）")
    dissolve = bool(opt(params, "dissolve", False))
    quad_segs = int(opt(params, "quad_segs", 16))
    gdf, _ = resolver.frame(need(params, "input", "proximity.buffer"))
    out = PROVIDER.buffer(gdf, distance, dissolve, quad_segs)
    if dissolve:
        text = f"缓冲区 {distance:g} m：融合为 1 个面（源 {len(gdf)} 个要素）"
    else:
        text = f"缓冲区 {distance:g} m：生成 {len(out)} 个面"
    return StepResult(out, [text])


def handle_within_distance(resolver, params: dict, step_id: str) -> StepResult:
    distance = float(need(params, "distance_m", "proximity.within_distance"))
    input_gdf, _ = resolver.frame(need(params, "input", "proximity.within_distance"))
    ref, _ = resolver.frame(need(params, "ref", "proximity.within_distance"))
    n_before = len(input_gdf)
    out = PROVIDER.within_distance(input_gdf, ref, distance)
    return StepResult(
        out,
        [f"邻近筛选：距参照层 {distance:g} m 以内的要素 {len(out)} 个（原 {n_before} 个）"],
    )


def handle_nearest(resolver, params: dict, step_id: str) -> StepResult:
    k = int(opt(params, "k", 1))
    join_attrs = list(opt(params, "join_attrs", []))
    input_gdf, _ = resolver.frame(need(params, "input", "proximity.nearest"))
    ref, _ = resolver.frame(need(params, "ref", "proximity.nearest"))
    out = PROVIDER.nearest(input_gdf, ref, k, join_attrs)
    far = float(out["nearest_distance_m"].max()) if len(out) else 0.0
    text = f"最近邻（k={k}）：输出 {len(out)} 行；最远最近距离 {far:,.0f} m"
    return StepResult(out, [text])


SPEC = {
    "proximity.buffer": handle_buffer,
    "proximity.within_distance": handle_within_distance,
    "proximity.nearest": handle_nearest,
}
